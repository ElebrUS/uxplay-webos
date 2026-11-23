#include "media_starfish.h"

#include <StarfishMediaAPIs.h>
#include <luna-service2/lunaservice.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sstream>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr int frame_ready = 0x00;
constexpr int load_completed = 0x15;
constexpr int end_of_stream = 0x1b;
constexpr int rockhopper_stream_error = 0xf002;
constexpr int aac_rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                             22050, 16000, 12000, 11025, 8000, 7350};

template <typename Function>
bool symbol(void *library, const char *name, Function &function) {
    function = reinterpret_cast<Function>(dlsym(library, name));
    if (!function) std::fprintf(stderr, "[starfish] unavailable symbol: %s\n", name);
    return function != nullptr;
}

bool success_reply(const char *payload) {
    if (!payload) return false;
    std::string compact;
    for (const unsigned char c : std::string(payload)) {
        if (!std::isspace(c)) compact += c;
    }
    return compact.find("\"returnValue\":true") != std::string::npos;
}

struct LunaError {
    LunaError() { LSErrorInit(&value); }
    ~LunaError() { LSErrorFree(&value); }
    LSError value;
};
}

struct StarfishVideo::Impl {
    using Callback = void (*)(int, int64_t, const char *);
    static std::atomic<Impl *> active;
    typename std::aligned_storage<sizeof(StarfishMediaAPIs), alignof(StarfishMediaAPIs)>::type storage;
    void *library = nullptr;
    void (*construct)(void *) = nullptr;
    void (*destroy)(void *) = nullptr;
    bool (*load)(void *, const char *, Callback) = nullptr;
    bool (*unload)(void *) = nullptr;
    bool (*play)(void *) = nullptr;
    bool (*pause)(void *) = nullptr;
    bool (*send_eos)(void *) = nullptr;
    const char *(*media_id)(void *) = nullptr;
    std::string (*send_packet)(void *, const char *) = nullptr;
    bool usable = false;
    bool constructed = false;
    bool loaded = false;
    bool connected = false;
    bool desired_pause = false;
    bool playing = false;
    bool attempted = false;
    int width = 0, height = 0, audio_index = -1, channels = 0;
    int64_t timestamp_origin = -1;
    std::string context;
    std::vector<uint8_t> adts;
    std::atomic<bool> load_ready{false}, failed{false}, ended{false};
    std::atomic<bool> stream_error{false};
    std::atomic<unsigned> frames{0};
    std::atomic<int64_t> relative_position{-1};
    GMainContext *luna_loop = nullptr;
    LSHandle *luna = nullptr;
    LSMessageToken luna_token = 0;
    enum class Plane { Idle, Connect, Window, Ready, Disconnect };
    Plane plane = Plane::Idle;
    Clock::time_point deadline{};

    static void event(int type, int64_t value, const char *message) {
        auto *self = active.load();
        if (!self) return;
        if (type == frame_ready) {
            self->relative_position = value;
            ++self->frames;
        } else if (type == load_completed) {
            self->load_ready = true;
        } else if (type == end_of_stream) {
            self->ended = true;
        } else if (type == 0x11 || type == 0x12) {
            std::fprintf(stderr, "[starfish] event error %lld %s\n",
                         static_cast<long long>(value), message ? message : "");
            self->stream_error = value == rockhopper_stream_error;
            self->failed = true;
        }
    }

    bool initialize() {
        if (attempted) return usable;
        attempted = true;
        library = dlopen("libplayerAPIs.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!library) {
            std::fprintf(stderr, "[starfish] %s\n", dlerror());
            return false;
        }
        usable = symbol(library, "_ZN17StarfishMediaAPIsC1Ev", construct) &&
            symbol(library, "_ZN17StarfishMediaAPIsD1Ev", destroy) &&
            symbol(library, "_ZN17StarfishMediaAPIs4LoadEPKcPFvixS1_E", load) &&
            symbol(library, "_ZN17StarfishMediaAPIs6UnloadEv", unload) &&
            symbol(library, "_ZN17StarfishMediaAPIs4PlayEv", play) &&
            symbol(library, "_ZN17StarfishMediaAPIs5PauseEv", pause) &&
            symbol(library, "_ZN17StarfishMediaAPIs7pushEOSEv", send_eos) &&
            symbol(library, "_ZN17StarfishMediaAPIs10getMediaIDEv", media_id) &&
            symbol(library, "_ZN17StarfishMediaAPIs4FeedEPKc", send_packet);
        return usable;
    }

    bool open_luna() {
        LunaError error;
        luna_loop = g_main_context_new();
        if (LSRegister(nullptr, &luna, &error.value) &&
            LSGmainContextAttach(luna, luna_loop, &error.value)) return true;
        std::fprintf(stderr, "[starfish] Luna setup failed: %s\n",
                     error.value.message ? error.value.message : "");
        return false;
    }

    static bool plane_reply(LSHandle *, LSMessage *message, void *opaque) {
        auto &state = *static_cast<Impl *>(opaque);
        state.luna_token = 0;
        const char *payload = LSMessageGetPayload(message);
        const bool ok = success_reply(payload);
        std::fprintf(stderr, "[starfish] plane step %d %s\n", int(state.plane), payload ? payload : "");
        switch (state.plane) {
        case Plane::Connect:
            if (!ok) state.failed = true;
            else {
                state.connected = true;
                state.set_window();
            }
            break;
        case Plane::Window:
            if (ok) state.plane = Plane::Ready;
            else state.failed = true;
            break;
        case Plane::Disconnect:
            state.connected = false;
            state.plane = Plane::Idle;
            break;
        default:
            break;
        }
        return true;
    }

    bool request(Plane step, const char *uri, const std::string &payload) {
        LunaError error;
        plane = step;
        deadline = Clock::now() + std::chrono::milliseconds(1500);
        if (LSCallOneReply(luna, uri, payload.c_str(), plane_reply, this, &luna_token, &error.value))
            return true;
        std::fprintf(stderr, "[starfish] Luna call failed: %s\n",
                     error.value.message ? error.value.message : "");
        failed = true;
        return false;
    }

    void set_window() {
        const int fit_width = std::min(1920, 1080 * width / height);
        const int fit_height = std::min(1080, 1920 * height / width);
        std::ostringstream payload;
        payload << "{\"context\":\"" << context << "\",\"fullScreen\":"
                << (fit_width == 1920 && fit_height == 1080 ? "true" : "false")
                << ",\"displayOutput\":{\"positionX\":" << (1920 - fit_width) / 2
                << ",\"positionY\":" << (1080 - fit_height) / 2
                << ",\"width\":" << fit_width << ",\"height\":" << fit_height << "}}";
        request(Plane::Window, "luna://com.webos.service.tv.display/setDisplayWindow", payload.str());
    }

    void dispatch() {
        for (int i = 0; luna_loop && i < 8 && g_main_context_iteration(luna_loop, FALSE); ++i) {}
    }

    std::string load_options(const char *app_id, const StarfishStream &stream) const {
        std::ostringstream payload;
        payload << "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{\"appId\":\"" << app_id
                << "\",\"queryPosition\":false,\"externalStreamingInfo\":{\"contents\":{\"codec\":{\"video\":\"H264\"";
        if (audio_index >= 0) payload << ",\"audio\":\"AAC\"";
        payload << "},\"esInfo\":{\"pauseAtDecodeTime\":false,\"ptsToDecode\":\"0\","
                   "\"seperatedPTS\":true,\"videoFpsValue\":" << stream.fps_num
                << ",\"videoFpsScale\":" << stream.fps_den << "},\"format\":\"RAW\",\"provider\":\"Chrome\"";
        if (audio_index >= 0)
            payload << ",\"aacInfo\":{\"format\":\"adts\",\"profile\":2,\"frequency\":"
                    << stream.audio_rate / 1000 << ",\"channels\":" << channels << "}";
        payload << "},\"restartStreaming\":false,\"streamQualityInfo\":true,"
                   "\"streamQualityInfoNonFlushable\":true,\"totalStreamSize\":256,"
                   "\"bufferingCtrInfo\":{\"preBufferByte\":0,\"bufferMinLevel\":0,\"bufferMaxLevel\":0,"
                   "\"qBufferLevelAudio\":0,\"qBufferLevelVideo\":0,"
                   "\"srcBufferLevelAudio\":{\"minimum\":1024,\"maximum\":1048576},"
                   "\"srcBufferLevelVideo\":{\"minimum\":1024,\"maximum\":8388608}}},"
                   "\"adaptiveStreaming\":{\"audioOnly\":false,\"maxWidth\":" << width
                << ",\"maxHeight\":" << height << ",\"maxFrameRate\":30}}}]}";
        return payload.str();
    }

    bool wrap_aac(const void *&data, size_t &size) {
        if (audio_index < 0 || size == 0 || size > 8184) return false;
        const auto *bytes = static_cast<const uint8_t *>(data);
        if (size >= 7 && bytes[0] == 0xff && (bytes[1] & 0xf6) == 0xf0) {
            const size_t frame_size = static_cast<size_t>((bytes[3] & 3) << 11 | bytes[4] << 3 | bytes[5] >> 5);
            if (frame_size == size) return true;
        }
        const size_t length = size + 7;
        adts.resize(length);
        adts[0] = 0xff;
        adts[1] = 0xf1;
        adts[2] = uint8_t(0x40 | (audio_index << 2) | (channels >> 2));
        adts[3] = uint8_t((channels & 3) << 6 | (length >> 11));
        adts[4] = uint8_t(length >> 3);
        adts[5] = uint8_t((length & 7) << 5 | 0x1f);
        adts[6] = 0xfc;
        std::memcpy(adts.data() + 7, bytes, size);
        data = adts.data();
        size = length;
        return true;
    }
};

std::atomic<StarfishVideo::Impl *> StarfishVideo::Impl::active{nullptr};

StarfishVideo::StarfishVideo() : impl_(new Impl) {}
// The library owns process-wide GStreamer state. Keep it loaded across clips.
StarfishVideo::~StarfishVideo() { close(); }

bool StarfishVideo::open(const char *app_id, int width, int height, const StarfishStream &stream) {
    if (!app_id || !*app_id || strspn(app_id, "abcdefghijklmnopqrstuvwxyz0123456789.") != strlen(app_id) ||
        width <= 0 || height <= 0 || width > 1920 || height > 1088) return false;
    auto &state = *impl_;
    if (!state.initialize()) return false;
    if (stream.audio_rate > 0) {
        const auto rate = std::find(std::begin(aac_rates), std::end(aac_rates), stream.audio_rate);
        if (rate == std::end(aac_rates) || stream.audio_channels < 1 || stream.audio_channels > 2) return false;
        state.audio_index = int(rate - std::begin(aac_rates));
        state.channels = stream.audio_channels;
    }
    Impl *expected = nullptr;
    if (!Impl::active.compare_exchange_strong(expected, &state)) return false;
    if (!state.open_luna()) {
        close();
        return false;
    }
    state.width = width;
    state.height = height;
    state.construct(&state.storage);
    state.constructed = true;
    state.loaded = state.load(&state.storage, state.load_options(app_id, stream).c_str(), Impl::event);
    if (!state.loaded) {
        std::fprintf(stderr, "[starfish] could not load the stream\n");
        close();
        return false;
    }
    state.deadline = Clock::now() + std::chrono::seconds(6);
    std::fprintf(stderr, "[starfish] stream loaded %dx%d aac=%d\n", width, height, state.audio_index >= 0);
    return true;
}

void StarfishVideo::tick() {
    auto &state = *impl_;
    if (!state.loaded || state.failed) return;
    state.dispatch();
    if (state.plane == Impl::Plane::Idle && state.load_ready) {
        const char *id = state.media_id(&state.storage);
        if (id && *id && strlen(id) < 128 &&
            strspn(id, "_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == strlen(id)) {
            state.context = id;
            state.request(Impl::Plane::Connect, "luna://com.webos.service.videosinkmanager/connect",
                          "{\"context\":\"" + state.context + "\",\"sinkType\":\"MAIN\"}");
        }
    }
    if (state.plane != Impl::Plane::Ready && Clock::now() >= state.deadline) {
        std::fprintf(stderr, "[starfish] timed out waiting for the video plane\n");
        state.failed = true;
    }
    if (state.plane == Impl::Plane::Ready && state.playing == state.desired_pause) {
        const auto action = state.desired_pause ? state.pause : state.play;
        if (action(&state.storage)) {
            state.playing = !state.desired_pause;
            std::fprintf(stderr, "[starfish] %s\n", state.playing ? "playing" : "paused");
        } else {
            state.failed = true;
        }
    }
}

bool StarfishVideo::opened() const { return impl_->loaded; }
bool StarfishVideo::failed() const { return impl_->failed; }
bool StarfishVideo::rockhopper_end_error() const { return impl_->stream_error; }
bool StarfishVideo::ended() const { return impl_->ended; }
unsigned StarfishVideo::frames() const { return impl_->frames; }
int64_t StarfishVideo::position() const {
    const int64_t relative = impl_->relative_position;
    return relative < 0 || impl_->timestamp_origin < 0 ? -1 : relative + impl_->timestamp_origin;
}

FeedResult StarfishVideo::feed(const void *data, size_t size, int64_t pts, bool video) {
    auto &state = *impl_;
    if (!state.loaded || state.failed) return FeedResult::Error;
    if (state.timestamp_origin < 0) state.timestamp_origin = std::max(int64_t(0), pts);
    if (!video && !state.wrap_aac(data, size)) return FeedResult::Error;
    char payload[240];
    std::snprintf(payload, sizeof(payload), "{\"bufferAddr\":\"%p\",\"bufferSize\":%zu,\"pts\":%lld,\"esData\":%d}",
                  data, size, static_cast<long long>(std::max(int64_t(0), pts - state.timestamp_origin)), video ? 1 : 2);
    const auto reply = state.send_packet(&state.storage, payload);
    if (reply.find("BufferFull") != std::string::npos || reply.find("Pending") != std::string::npos)
        return FeedResult::Retry;
    if (reply.find("Ok") != std::string::npos) return FeedResult::Accepted;
    std::fprintf(stderr, "[starfish] feed rejected: %s\n", reply.c_str());
    return FeedResult::Error;
}

void StarfishVideo::pause(bool paused) { impl_->desired_pause = paused; }
void StarfishVideo::eos() { if (impl_->loaded) impl_->send_eos(&impl_->storage); }

void StarfishVideo::close() {
    auto &state = *impl_;
    if (state.luna) {
        LunaError error;
        if (state.luna_token) LSCallCancel(state.luna, state.luna_token, &error.value);
        if (state.connected) {
            state.request(Impl::Plane::Disconnect, "luna://com.webos.service.videosinkmanager/disconnect",
                          "{\"context\":\"" + state.context + "\"}");
            const auto until = Clock::now() + std::chrono::milliseconds(250);
            while (state.connected && Clock::now() < until) {
                state.dispatch();
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        }
        LSUnregister(state.luna, &error.value);
        state.luna = nullptr;
    }
    if (state.luna_loop) {
        g_main_context_unref(state.luna_loop);
        state.luna_loop = nullptr;
    }
    if (state.loaded) state.unload(&state.storage);
    if (state.constructed) state.destroy(&state.storage);
    Impl *expected = &state;
    Impl::active.compare_exchange_strong(expected, nullptr);
    state.loaded = state.constructed = state.connected = state.playing = state.desired_pause = false;
    state.luna_token = 0;
    state.plane = Impl::Plane::Idle;
    state.context.clear();
    state.audio_index = -1;
    state.timestamp_origin = -1;
    state.load_ready = state.failed = state.ended = false;
    state.stream_error = false;
    state.frames = 0;
    state.relative_position = -1;
}
