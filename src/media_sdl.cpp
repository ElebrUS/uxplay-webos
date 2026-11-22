#include "media.h"
#ifdef WEBOS14_LGNC
#include "media_lgnc.h"
#endif
#include <SDL.h>
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <vector>

struct Media::Impl {
    explicit Impl(SDL_Renderer *output) : renderer(output) {}
    SDL_Renderer *renderer;
    SDL_Texture *texture = nullptr;
    double aspect = 1;
    SDL_Texture *loading_texture = nullptr;
    AVFrame *last_frame = nullptr;
    bool loading = false;
    bool hold_hardware_frame = false;
    double loading_aspect = 1;
    std::function<void()> overlay;
    AVCodecContext *decoder = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    int64_t first_visible_pts = std::numeric_limits<int64_t>::min();
    int64_t playback_pts = 0;
    bool clock_active = false;
    Uint32 last_presented_at = 0;
    using UpdateYuv = decltype(&SDL_UpdateYUVTexture);
    UpdateYuv update_yuv = reinterpret_cast<UpdateYuv>(dlsym(RTLD_DEFAULT, "SDL_UpdateYUVTexture"));
    std::vector<uint8_t> planar;
    int width = 0;
    int height = 0;
    SDL_AudioDeviceID audio = 0;
    std::array<uint8_t, 44100 * 4 / 5> pcm{};
    size_t audio_read = 0;
    size_t audio_size = 0;
#ifdef WEBOS14_LGNC
    LgncVideo hardware;
#endif

    bool hardware_video() const {
#ifdef WEBOS14_LGNC
        return hardware.opened();
#else
        return false;
#endif
    }

    static void audio_callback(void *context, Uint8 *output, int size) {
        auto &self = *static_cast<Impl *>(context);
        std::memset(output, 0, size);
        const size_t count = std::min(size_t(size), self.audio_size);
        const size_t first = std::min(count, self.pcm.size() - self.audio_read);
        std::memcpy(output, self.pcm.data() + self.audio_read, first);
        std::memcpy(output + first, self.pcm.data(), count - first);
        self.audio_read = (self.audio_read + count) % self.pcm.size();
        self.audio_size -= count;
    }

    void open_audio(bool enabled, int rate) {
        if (!enabled) return;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            SDL_AudioSpec spec{};
            spec.freq = rate;
            spec.format = AUDIO_S16LSB;
            spec.channels = 2;
            spec.samples = 512;
            spec.callback = audio_callback;
            spec.userdata = this;
            audio = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);
            if (audio) SDL_PauseAudioDevice(audio, 0);
        }
        if (!audio) std::fprintf(stderr, "[audio] SDL unavailable: %s\n", SDL_GetError());
    }

    int upload() {
        if (update_yuv) {
            return update_yuv(texture, nullptr, frame->data[0], frame->linesize[0],
                              frame->data[1], frame->linesize[1],
                              frame->data[2], frame->linesize[2]);
        }
        // SDL 2.0.0 accepts contiguous IYUV through SDL_UpdateTexture.
        const int chroma_width = (width + 1) / 2;
        const int chroma_height = (height + 1) / 2;
        planar.resize(size_t(width) * height + 2 * size_t(chroma_width) * chroma_height);
        uint8_t *dst = planar.data();
        for (int plane = 0; plane < 3; ++plane) {
            const int pitch = plane == 0 ? width : chroma_width;
            const int rows = plane == 0 ? height : chroma_height;
            const uint8_t *src = frame->data[plane];
            for (int row = 0; row < rows; ++row) {
                std::memcpy(dst, src, pitch);
                dst += pitch;
                src += frame->linesize[plane];
            }
        }
        return SDL_UpdateTexture(texture, nullptr, planar.data(), width);
    }

    bool present() {
        if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) {
            std::fprintf(stderr, "[video] unsupported pixel format %d\n", frame->format);
            return false;
        }
        if (!texture || width != frame->width || height != frame->height) {
            if (texture) SDL_DestroyTexture(texture);
            width = frame->width;
            height = frame->height;
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                        SDL_TEXTUREACCESS_STREAMING, width, height);
            if (!texture) return false;
        }
        if (upload() != 0) return false;
        const auto sar = frame->sample_aspect_ratio;
        aspect = double(width) / height * (sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0);
        if (!last_frame) last_frame = av_frame_alloc();
        if (last_frame) {
            av_frame_unref(last_frame);
            av_frame_ref(last_frame, frame);
        }
        return redraw();
    }

    bool redraw() {
        SDL_Texture *picture = loading ? loading_texture : texture;
        if (!picture && !loading && !hardware_video()) return false;
        int output_width, output_height;
        if (SDL_GetRendererOutputSize(renderer, &output_width, &output_height) != 0 ||
            output_width <= 0 || output_height <= 0) return false;
        const double display_aspect = loading ? loading_aspect : aspect;
        const int fitted_width = std::max(1, int(std::min(double(output_width), output_height * display_aspect)));
        const int fitted_height = std::max(1, int(std::min(double(output_height), output_width / display_aspect)));
        const SDL_Rect target = {(output_width - fitted_width) / 2, (output_height - fitted_height) / 2,
                                 fitted_width, fitted_height};
        if (!picture || fitted_width != output_width || fitted_height != output_height) {
            const bool transparent = !picture && hardware_video() && (!loading || hold_hardware_frame);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, transparent ? 0 : 255);
            if (SDL_RenderClear(renderer) != 0) return false;
        }
        if (picture && SDL_RenderCopy(renderer, picture, nullptr, &target) != 0) return false;
        if (overlay) overlay();
        SDL_RenderPresent(renderer);
        return true;
    }
};

Media::Media(SDL_Renderer *renderer) : impl_(new Impl(renderer)) {}
void Media::set_overlay(std::function<void()> overlay) { impl_->overlay = std::move(overlay); }
void Media::redraw() { impl_->redraw(); }
void Media::begin_loading(bool preserve_frame) {
    auto &state = *impl_;
    if (!state.loading) {
        state.loading = true;
        state.hold_hardware_frame = preserve_frame && hardware_video() && frames > 0;
        state.loading_aspect = state.aspect;
        const AVFrame *frame = state.last_frame;
        if (preserve_frame && frame && frame->data[0]) {
            // Average each source block, then upscale the small image with linear filtering.
            constexpr int w = 40, h = 24;
            std::array<uint8_t, w * h * 3 / 2> pixels{};
            size_t output = 0;
            for (int plane = 0; plane < 3; ++plane) {
                const int shift = plane != 0;
                const int in_w = (frame->width + shift) >> shift;
                const int in_h = (frame->height + shift) >> shift;
                const int out_w = w >> shift, out_h = h >> shift;
                for (int y = 0; y < out_h; ++y) {
                    const int top = y * in_h / out_h;
                    const int bottom = std::min(in_h, std::max(top + 1, (y + 1) * in_h / out_h));
                    for (int x = 0; x < out_w; ++x) {
                        const int left = x * in_w / out_w;
                        const int right = std::min(in_w, std::max(left + 1, (x + 1) * in_w / out_w));
                        unsigned sum = 0;
                        for (int row = top; row < bottom; ++row) {
                            const auto *line = frame->data[plane] + row * frame->linesize[plane];
                            for (int column = left; column < right; ++column) sum += line[column];
                        }
                        pixels[output++] = sum / ((bottom - top) * (right - left));
                    }
                }
            }
            state.loading_texture = SDL_CreateTexture(state.renderer, SDL_PIXELFORMAT_IYUV,
                                                       SDL_TEXTUREACCESS_STREAMING, w, h);
            if (state.loading_texture &&
                SDL_UpdateTexture(state.loading_texture, nullptr, pixels.data(), w) != 0) {
                SDL_DestroyTexture(state.loading_texture);
                state.loading_texture = nullptr;
            }
        }
    }
    redraw();
}
void Media::end_loading() {
    auto &state = *impl_;
    if (state.loading_texture) SDL_DestroyTexture(state.loading_texture);
    state.loading_texture = nullptr;
    state.loading = false;
    state.hold_hardware_frame = false;
}
Media::~Media() { end_loading(); close(); delete impl_; }

bool Media::open(const char *, int width, int height, bool audio, int rate) {
    return open_decoder(width, height, audio, rate, false);
}

bool Media::open_clip(int width, int height, bool audio, const char *hardware_app_id) {
#ifdef WEBOS14_LGNC
    if (hardware_app_id && width > 0 && height > 0) {
        close();
        auto &state = *impl_;
        if (state.hardware.open(hardware_app_id, width, height)) {
            state.width = width;
            state.height = height;
            state.open_audio(audio, 44100);
            ready = playing = true;
            return true;
        }
    }
#else
    (void)hardware_app_id;
#endif
    return open_decoder(width, height, audio, 44100, true);
}

void Media::show_frames_from(int64_t pts_ns) { impl_->first_visible_pts = pts_ns; }
void Media::set_playback_clock(int64_t pts_ns) {
    impl_->playback_pts = pts_ns;
    impl_->clock_active = true;
}
bool Media::hardware_video() const { return impl_->hardware_video(); }
bool Media::hardware_enabled() const {
#ifdef WEBOS14_LGNC
    const char *setting = std::getenv("UXPLAY_LGNC");
    return !setting || std::strcmp(setting, "0") != 0;
#else
    return false;
#endif
}
int Media::video_width() const { return impl_->texture || (hardware_video() && frames) ? impl_->width : 0; }
int Media::video_height() const { return impl_->texture || (hardware_video() && frames) ? impl_->height : 0; }

bool Media::open_decoder(int width, int height, bool audio, int rate, bool buffered) {
    close();
    auto &state = *impl_;
    if (!state.renderer || width <= 0 || height <= 0) return false;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return false;
    state.decoder = avcodec_alloc_context3(codec);
    state.frame = av_frame_alloc();
    state.packet = av_packet_alloc();
    if (!state.decoder || !state.frame || !state.packet) {
        close();
        return false;
    }
    state.decoder->max_pixels = 1920 * 1088;
    state.decoder->thread_count = 2;
    state.decoder->thread_type = FF_THREAD_SLICE;
    if (buffered) state.decoder->thread_type |= FF_THREAD_FRAME;
    else state.decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(state.decoder, codec, nullptr) < 0) {
        close();
        return false;
    }
    state.open_audio(audio, rate);
    ready = playing = true;
    return true;
}

FeedResult Media::feed(const void *data, size_t size, int64_t pts_ns, bool video, bool present_video) {
    auto &state = *impl_;
    if (!ready || !data || size == 0 || size > INT_MAX) return FeedResult::Error;
    if (!video) {
        if (!state.audio) return FeedResult::Accepted;
        if (size > state.pcm.size() || size % 4) return FeedResult::Error;
        SDL_LockAudioDevice(state.audio);
        if (size > state.pcm.size() - state.audio_size) {
            SDL_UnlockAudioDevice(state.audio);
            return FeedResult::Retry;
        }
        const size_t end = (state.audio_read + state.audio_size) % state.pcm.size();
        const size_t first = std::min(size, state.pcm.size() - end);
        std::memcpy(state.pcm.data() + end, data, first);
        std::memcpy(state.pcm.data(), static_cast<const uint8_t *>(data) + first, size - first);
        state.audio_size += size;
        SDL_UnlockAudioDevice(state.audio);
        return FeedResult::Accepted;
    }

#ifdef WEBOS14_LGNC
    if (hardware_video()) {
        const auto result = state.hardware.feed(data, size);
        if (result == FeedResult::Accepted && present_video && pts_ns >= state.first_visible_pts)
            ++frames;
        return result;
    }
#endif
    if (av_new_packet(state.packet, int(size)) < 0) return FeedResult::Error;
    std::memcpy(state.packet->data, data, size);
    state.packet->pts = pts_ns;
    const int result = avcodec_send_packet(state.decoder, state.packet);
    av_packet_unref(state.packet);
    if (result < 0) {
        std::fprintf(stderr, "[video] H.264 packet rejected: %d\n", result);
        return result == AVERROR_INVALIDDATA ? FeedResult::Accepted : FeedResult::Error;
    }
    return receive_frames(present_video);
}

FeedResult Media::receive_frames(bool present_video) {
    auto &state = *impl_;
    int result;
    while ((result = avcodec_receive_frame(state.decoder, state.frame)) == 0) {
        const Uint32 now = SDL_GetTicks();
        // Decode reference frames normally. Skip expensive uploads only while catching up.
        const bool late = state.clock_active && state.frame->pts != AV_NOPTS_VALUE &&
            state.frame->pts < state.playback_pts - 120000000;
        const bool visible = present_video && state.frame->pts >= state.first_visible_pts &&
            (!late || frames == 0 || now - state.last_presented_at >= 100);
        const bool ok = !visible || state.present();
        av_frame_unref(state.frame);
        if (!ok) {
            std::fprintf(stderr, "[video] SDL presentation failed: %s\n", SDL_GetError());
            return FeedResult::Error;
        }
        if (visible) {
            ++frames;
            state.last_presented_at = now;
        }
    }
    return result == AVERROR(EAGAIN) || result == AVERROR_EOF || result == AVERROR_INVALIDDATA ?
        FeedResult::Accepted : FeedResult::Error;
}

void Media::tick() {}
void Media::eos() {
    impl_->clock_active = false;
    if (impl_->decoder && avcodec_send_packet(impl_->decoder, nullptr) == 0)
        failed = receive_frames(true) == FeedResult::Error;
}
void Media::pause(bool paused) {
    if (impl_->audio) SDL_PauseAudioDevice(impl_->audio, paused);
}
void Media::close() {
    auto &state = *impl_;
#ifdef WEBOS14_LGNC
    state.hardware.close();
#endif
    state.hold_hardware_frame = false;
    if (state.audio) SDL_CloseAudioDevice(state.audio);
    state.audio = 0;
    state.audio_size = state.audio_read = 0;
    if (state.texture) SDL_DestroyTexture(state.texture);
    state.texture = nullptr;
    std::vector<uint8_t>().swap(state.planar);
    av_packet_free(&state.packet);
    av_frame_free(&state.last_frame);
    av_frame_free(&state.frame);
    avcodec_free_context(&state.decoder);
    state.first_visible_pts = std::numeric_limits<int64_t>::min();
    state.clock_active = false;
    ready = playing = ended = failed = false;
    frames = 0;
}
