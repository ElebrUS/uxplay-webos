#include "direct_clip.h"
#include "audio.h"
#include "media.h"
#include "shell.h"
#include "hls_playlist.h"
extern "C" {
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <iterator>
#include <limits>
#include <fstream>
#include <memory>

namespace {
constexpr int64_t ns_per_second = 1000000000;
constexpr int64_t ns_per_ms = 1000000;
constexpr int64_t timeout_ns = 5 * ns_per_second;
constexpr size_t max_packet_size = 1024 * 1024;
constexpr size_t max_queue_packets = 128;
constexpr size_t max_queue_bytes = 8 * 1024 * 1024;
constexpr int64_t rebuffer_delay = 120 * ns_per_ms;
constexpr int64_t resume_buffer = 350 * ns_per_ms;

bool http_url(const std::string &url) {
    if (url.size() > 8192 || url.find_first_of(" \t\r\n\\") != std::string::npos) return false;
    const auto authority = url.compare(0, 8, "https://") == 0 ? 8 :
                           url.compare(0, 7, "http://") == 0 ? 7 : 0;
    if (!authority) return false;
    const auto end = url.find_first_of("/?#", authority);
    const auto host = url.substr(authority, end - authority);
    return !host.empty() && host.find('@') == std::string::npos;
}

enum class StreamKind { Mp4, Hls, Dash };

StreamKind stream_kind(const std::string &url) {
    auto path = url.substr(0, url.find_first_of("?#"));
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return std::tolower(c); });
    if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".m3u8") == 0) return StreamKind::Hls;
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".mpd") == 0) return StreamKind::Dash;
    return StreamKind::Mp4;
}

int video_stream(const AVFormatContext &input) {
    constexpr int preferred = 640 * 360;
    int choice = -1;
    int best_score = std::numeric_limits<int>::min();
    for (unsigned i = 0; i < input.nb_streams; ++i) {
        const auto &stream = *input.streams[i];
        const auto &codec = *stream.codecpar;
        if (stream.disposition & AV_DISPOSITION_ATTACHED_PIC || codec.codec_id != AV_CODEC_ID_H264 ||
            codec.width <= 0 || codec.height <= 0 || codec.width > 1920 || codec.height > 1088) continue;
        const int pixels = codec.width * codec.height;
        const int score = pixels <= preferred ? pixels : preferred - (pixels - preferred);
        if (score > best_score) { choice = int(i); best_score = score; }
    }
    return choice;
}

void secure_http_options(AVDictionary **options) {
    av_dict_set(options, "protocol_whitelist", "http,https,tcp,tls", 0);
    av_dict_set(options, "tls_verify", "1", 0);
    av_dict_set(options, "ca_file", "/etc/ssl/certs/ca-certificates.crt", 0);
}

void free_filter(AVBSFContext *ctx) { av_bsf_free(&ctx); }
void free_packet(AVPacket *packet) { av_packet_free(&packet); }
}

DirectClip::DirectClip() {
    std::ifstream config(app_directory() + "/safari-source.txt");
    std::string expected, media;
    std::getline(config, expected);
    std::getline(config, media);
    if (const char *value = getenv("UXPLAY_DIRECT_URL")) expected = value;
    if (const char *value = getenv("UXPLAY_DIRECT_MEDIA_URL")) media = value;
    if (http_url(expected)) profile_url_ = expected;
    if (http_url(media)) profile_media_ = media;
    else profile_media_ = profile_url_;
}
DirectClip::~DirectClip() { join(); }

bool DirectClip::play(const char *url, double start) {
    const std::string request = url && strnlen(url, 8193) <= 8192 ? url : "";
    const auto suffix = request.find_first_of("?#");
    const bool mapped = !profile_url_.empty() && !profile_media_.empty() &&
        (request == profile_url_ || (profile_media_ != profile_url_ && request.substr(0, suffix) == profile_url_));
    const std::string source = mapped ? profile_media_ : request.substr(0, request.find('#'));
    const auto kind = stream_kind(source);
    const bool accepted = http_url(request) && kind != StreamKind::Dash;
    const char *format = kind == StreamKind::Hls ? "HLS" : kind == StreamKind::Dash ? "DASH" : "MP4";
    fprintf(stderr, "[direct] play request accepted=%d profile=%d format=%s\n",
            int(accepted), int(mapped), format);
    if (!accepted) return false;
    std::lock_guard<std::mutex> lock(requests_);
    authorized_clip_ = true;
    requested_source_ = source;
    fallback_source_ = mapped && source != request && stream_kind(request) == StreamKind::Hls ?
        request.substr(0, request.find('#')) : "";
    new_clip_ = true;
    reopen_input_ = false;
    forward_keyframe_ = false;
    requested_position_ = std::isfinite(start) && start >= 0 && start <= 36000 ? start : 0;
    start_requested_ = true;
    stop_requested_ = requested_pause_ = false;
    return true;
}

void DirectClip::seek(double position) {
    if (!std::isfinite(position) || position < 0 || position > 36000) return;
    std::lock_guard<std::mutex> lock(requests_);
    if (!authorized_clip_ || (ready_ && duration_ms_ <= 0)) return;
    const int current_ms = position_ms_.load();
    const double current = current_ms < 0 ? requested_position_ : current_ms / 1000.0;
    if (std::fabs(current - position) < 1.5 ||
        (current_ms < 5000 && std::fabs(requested_position_ - position) < 0.05)) return;
    fprintf(stderr, "[direct] seek %.2f seconds\n", position);
    requested_position_ = position;
    forward_keyframe_ = false;
    start_requested_ = true;
    stop_requested_ = false;
}
void DirectClip::pause(bool value) {
    std::lock_guard<std::mutex> lock(requests_);
    requested_pause_ = value;
}
bool DirectClip::remote(Action action, Media &media, SDL_Point pointer) {
    if (action == Action::PointerMove || action == Action::PointerClick) {
        const auto now = now_ns();
        const bool visible = controls_until_ > now || paused_ || scrub_seconds_ >= 0;
        controls_until_ = now + 4 * ns_per_second;
        const int previous_hover = pointer_seconds_;
        const double duration = duration_ms_.load() / 1000.0;
        const bool on_timeline = pointer.y >= 912 && pointer.y <= 950 &&
            pointer.x >= 467 && pointer.x <= 1453 && duration > 0;
        const double target = on_timeline ? std::min(duration - 0.001, (pointer.x - 467) * duration / 986) : -1;
        pointer_seconds_ = playing_ && !loading_ && quality_index_ < 0 && on_timeline ? int(target) : -1;
        pointer_until_ = now + 3 * ns_per_second;
        if (action == Action::PointerClick && visible && playing_ && !loading_ && quality_index_ < 0) {
            if (on_timeline) {
                scrub_seconds_ = -1;
                scrub_commit_at_ = 0;
                seek(target);
            } else if (pointer.y >= 825 && pointer.y <= 903) {
                if (pointer.x >= 921 && pointer.x <= 999) action = Action::Select;
                if (duration > 0 && (std::abs(pointer.x - 810) <= 39 || std::abs(pointer.x - 1110) <= 39)) {
                    scrub_seconds_ = -1;
                    scrub_commit_at_ = 0;
                    const double position = std::max(0, position_ms_.load()) / 1000.0;
                    seek(std::max(0.0, std::min(duration - 0.001, position + (pointer.x < 960 ? -10 : 10))));
                }
            }
        }
        if (action != Action::Select) {
            if (action == Action::PointerClick || !visible || pointer_seconds_ != previous_hover) media.redraw();
            return true;
        }
    }
    if (quality_remote(action)) {
        media.redraw();
        return true;
    }
    if (action == Action::SeekRelease) {
        if (scrub_commit_at_) scrub_commit_at_ = now_ns() + 180 * ns_per_ms;
        return true;
    }
    if (action == Action::Back && scrub_seconds_ >= 0) {
        scrub_seconds_ = -1;
        scrub_commit_at_ = 0;
        controls_until_ = now_ns() + 4 * ns_per_second;
        media.redraw();
        return true;
    }
    const bool repeated = action == Action::LeftRepeat || action == Action::RightRepeat;
    const bool left = action == Action::Left || action == Action::LeftRepeat;
    const bool right = action == Action::Right || action == Action::RightRepeat;
    if (action != Action::Select && !left && !right) return false;
    int commit = -1;
    {
        std::lock_guard<std::mutex> lock(requests_);
        if (!authorized_clip_) return false;
        const auto now = now_ns();
        controls_until_ = now + 4 * ns_per_second;
        if (action == Action::Select) {
            if (scrub_seconds_ >= 0) {
                commit = scrub_seconds_;
                scrub_seconds_ = -1;
                scrub_commit_at_ = 0;
            } else {
                requested_pause_ = !requested_pause_;
                fprintf(stderr, "[remote] %s\n", requested_pause_ ? "pause" : "resume");
            }
        } else {
            const int duration = duration_ms_ > 0 ? duration_ms_.load() / 1000 : loading_duration_;
            if (duration <= 0 || (repeated && scrub_seconds_ < 0)) return true;
            const int position = position_ms_;
            if (scrub_seconds_ < 0) {
                scrub_seconds_ = position < 0 ? int(requested_position_) : position / 1000;
                scrub_commit_at_ = now + 800 * ns_per_ms;
            }
            if (repeated) scrub_commit_at_ = 0;
            if (!repeated) {
                hold_started_ = now;
                if (scrub_commit_at_) scrub_commit_at_ = now + 800 * ns_per_ms;
            }
            if (repeated && now - last_scrub_step_ < 130 * ns_per_ms) return true;
            last_scrub_step_ = now;
            const auto held = now - hold_started_;
            const int step = repeated && held >= 2 * ns_per_second ? 60 :
                             repeated && held >= ns_per_second ? 30 : 10;
            scrub_seconds_ = std::max(0, std::min(duration - 1, scrub_seconds_ + (left ? -step : step)));
        }
    }
    if (commit >= 0) {
        fprintf(stderr, "[remote] seek confirmed at %d seconds\n", commit);
        seek(commit);
    }
    media.redraw();
    return true;
}

bool DirectClip::quality_remote(Action action) {
    if (action == Action::None || action == Action::SeekRelease) return false;
    if (quality_index_ < 0 && action != Action::Up) return false;
    if (quality_index_ >= 0 && action == Action::Back) {
        quality_index_ = -1;
        return true;
    }
    std::lock_guard<std::mutex> lock(requests_);
    if (!authorized_clip_ || start_requested_ || !playing_ || scrub_seconds_ >= 0) return false;
    if (loading_ && quality_index_ < 0) return false;
    controls_until_ = now_ns() + 4 * ns_per_second;
    if (quality_index_ < 0) {
        if (duration_ms_ <= 0 || rendition_heights_.size() < 2) return true;
        quality_choices_ = rendition_heights_;
        quality_choices_.insert(quality_choices_.begin(), 0);
        const auto selected = std::find(quality_choices_.begin(), quality_choices_.end(), quality_height_);
        quality_index_ = selected == quality_choices_.end() ? 0 : int(selected - quality_choices_.begin());
    } else if (action == Action::Select) {
        const int selected = quality_choices_[quality_index_];
        quality_index_ = -1;
        if (selected != quality_height_) {
            next_quality_height_ = selected;
            requested_position_ = std::max(0, position_ms_.load()) / 1000.0;
            reopen_input_ = start_requested_ = true;
            fprintf(stderr, "[direct] quality requested height=%d at %.2f s\n", selected, requested_position_);
        }
    } else if (action == Action::Up || action == Action::Down) {
        const int count = int(quality_choices_.size());
        quality_index_ = (quality_index_ + (action == Action::Down ? 1 : count - 1)) % count;
    } else {
        // Keep normal seek actions disabled while the quality picker has focus.
        return true;
    }
    return true;
}

void DirectClip::draw_controls(Shell &shell, const Media &media) const {
    if (!loading_ && playing_ && !paused_ && quality_index_ < 0 &&
        scrub_seconds_ < 0 && now_ns() >= controls_until_) return;
    const int width = media.video_width(), height = media.video_height();
    const std::string resolution = width > 0 ? std::to_string(width) + " x " + std::to_string(height) +
        (media.starfish_video() ? "  Starfish" : media.hardware_video() ? "  HW" : "") : "";
    if (quality_index_ >= 0) {
        const int selection = quality_choices_[quality_index_];
        shell.playback_quality(selection > 0 ? std::to_string(selection) + "p" : "Auto",
                               resolution.empty() ? "" : "Playing " + resolution, selection >= 720);
        return;
    }
    if (loading_) shell.playback_loader();
    if (scrub_seconds_ >= 0) {
        const int duration = duration_ms_ > 0 ? duration_ms_.load() / 1000 : loading_duration_;
        shell.playback_controls(position_ms_ / 1000, duration, true, scrub_seconds_, !scrub_commit_at_, resolution);
    } else if (loading_ && loading_target_ >= 0 && loading_duration_ > 0) {
        shell.playback_controls(loading_target_, loading_duration_, false);
    } else if (playing_ && !loading_ && (paused_ || now_ns() < controls_until_)) {
        const std::string label = multiple_renditions_ && duration_ms_ > 0 ?
            resolution + "  |  UP: Quality" : resolution;
        shell.playback_controls(position_ms_ / 1000, duration_ms_ / 1000, paused_, -1, false, label,
                                now_ns() < pointer_until_ ? pointer_seconds_ : -1);
    }
}

void DirectClip::stop() {
    std::lock_guard<std::mutex> lock(requests_);
    authorized_clip_ = false;
    stop_requested_ = true;
    start_requested_ = false;
}

void DirectClip::join(bool keep_input) {
    interrupted_ = true;
    available_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (!keep_input) close_input();
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    queued_bytes_ = 0;
    ready_ = false;
    duration_ms_ = position_ms_ = -1;
}

void DirectClip::close_input() {
    avformat_close_input(&input_);
    if (playlist_io_) {
        av_freep(&playlist_io_->buffer);
        avio_context_free(&playlist_io_);
    }
    playlist_text_.clear();
    playlist_offset_ = 0;
}

bool DirectClip::load_playlist(const std::string &url, std::string &resolved) {
    constexpr size_t max_playlist = 256 * 1024;
    AVIOContext *network = nullptr;
    AVDictionary *options = nullptr;
    secure_http_options(&options);
    const int opened = avio_open2(&network, url.c_str(), AVIO_FLAG_READ, &input_->interrupt_callback, &options);
    av_dict_free(&options);
    if (opened < 0) return false;

    uint8_t *location = nullptr;
    if (av_opt_get(network, "location", AV_OPT_SEARCH_CHILDREN, &location) >= 0) {
        const char *text = reinterpret_cast<const char *>(location);
        if (text && http_url(text)) resolved = text;
        av_free(location);
    }
    char buffer[4096];
    int count;
    while ((count = avio_read(network, reinterpret_cast<uint8_t *>(buffer), sizeof(buffer))) > 0) {
        if (playlist_text_.size() + size_t(count) > max_playlist) break;
        playlist_text_.append(buffer, count);
    }
    avio_closep(&network);
    if (count != AVERROR_EOF || playlist_text_.compare(0, 7, "#EXTM3U") != 0) return false;
    HlsSelection selection;
    const auto filtered = select_hls_rendition(playlist_text_, quality_height_, &selection, prefer_hardware_hls_);
    promoted_hls_ = selection.promoted_for_hardware;
    {
        std::lock_guard<std::mutex> lock(requests_);
        multiple_renditions_ = selection.heights.size() >= 2;
        rendition_heights_ = std::move(selection.heights);
    }
    if (filtered != playlist_text_)
        fprintf(stderr, "[direct] selected H.264 variant, hardware preference=%d\n", int(promoted_hls_));
    playlist_text_ = filtered;

    auto *scratch = static_cast<uint8_t *>(av_malloc(4096));
    if (!scratch) return false;
    playlist_io_ = avio_alloc_context(scratch, 4096, 0, this,
        [](void *opaque, uint8_t *dst, int length) -> int {
            auto &clip = *static_cast<DirectClip *>(opaque);
            const size_t remaining = clip.playlist_text_.size() - clip.playlist_offset_;
            if (!remaining) return AVERROR_EOF;
            const size_t count = std::min(size_t(length), remaining);
            std::memcpy(dst, clip.playlist_text_.data() + clip.playlist_offset_, count);
            clip.playlist_offset_ += count;
            return int(count);
        }, nullptr, nullptr);
    if (!playlist_io_) { av_free(scratch); return false; }
    input_->pb = playlist_io_;
    input_->flags |= AVFMT_FLAG_CUSTOM_IO;
    return true;
}

bool DirectClip::enqueue(Sample sample) {
    std::unique_lock<std::mutex> lock(mutex_);
    available_.wait(lock, [&] {
        return interrupted_ || (queue_.size() < max_queue_packets &&
            queued_bytes_ + sample.data.size() <= max_queue_bytes);
    });
    if (interrupted_) return false;
    queued_bytes_ += sample.data.size();
    // MOV can return nearby audio and video chunks in file order. Schedule them
    // by decode time without changing packet order within either track.
    auto position = queue_.end();
    while (position != queue_.begin()) {
        const auto previous = std::prev(position);
        if (previous->video == sample.video || previous->decode_at <= sample.decode_at) break;
        position = previous;
    }
    queue_.insert(position, std::move(sample));
    return true;
}

void DirectClip::read(double start, std::string source, bool forward_keyframe) {
    const bool needs_seek = input_ || start > 0;
    auto demux = [&] {
        if (!input_) {
            input_ = avformat_alloc_context();
            if (!input_) return false;
            input_->interrupt_callback.callback = [](void *opaque) -> int {
                auto &clip = *static_cast<DirectClip *>(opaque);
                return clip.interrupted_ || now_ns() > clip.deadline_;
            };
            input_->interrupt_callback.opaque = this;
            input_->io_open = [](AVFormatContext *context, AVIOContext **io, const char *url,
                                  int flags, AVDictionary **options) -> int {
                if (!url || !http_url(url)) return AVERROR(EINVAL);
                AVDictionary *defaults = nullptr;
                auto **args = options ? options : &defaults;
                secure_http_options(args);
                const int result = avio_open2(io, url, flags, &context->interrupt_callback, args);
                av_dict_free(&defaults);
                return result;
            };
            std::string resolved = source;
            const bool hls = stream_kind(source) == StreamKind::Hls;
            if (hls) {
                deadline_ = now_ns() + timeout_ns;
                if (!load_playlist(source, resolved)) return false;
            }
            AVDictionary *options = nullptr;
            secure_http_options(&options);
            av_dict_set(&options, "probesize", "1048576", 0);
            av_dict_set(&options, "analyzeduration", "2000000", 0);
            av_dict_set(&options, "http_multiple", "0", 0);
            deadline_ = now_ns() + (stream_kind(source) == StreamKind::Hls ? 12 * ns_per_second : timeout_ns);
            const int opened = avformat_open_input(&input_, resolved.c_str(), hls ? av_find_input_format("hls") : nullptr, &options);
            av_dict_free(&options);
            if (opened < 0) {
                if (!interrupted_) fprintf(stderr, "[direct] open failed: %d\n", opened);
                return false;
            }
            if (avformat_find_stream_info(input_, nullptr) < 0 ||
                input_->duration / 1000 >= std::numeric_limits<int>::max()) return false;
            fprintf(stderr, "[direct] opened container %s\n", input_->iformat->name);
        } else {
            fprintf(stderr, "[direct] reusing stream index for seek\n");
        }
        AVFormatContext *input = input_;

        const int video = video_stream(*input);
        if (video < 0) return false;
        const int audio = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, video, nullptr, 0);
        for (unsigned i = 0; i < input->nb_streams; ++i) {
            if (int(i) != video && int(i) != audio) input->streams[i]->discard = AVDISCARD_ALL;
        }
        const AVCodecParameters *video_params = input->streams[video]->codecpar;
        if (video_params->codec_id != AV_CODEC_ID_H264) return false;
        const int profile = video_params->profile;
        const AVRational frame_rate = av_guess_frame_rate(input, input->streams[video], nullptr);
        fprintf(stderr, "[direct] nominal video rate=%d/%d\n", frame_rate.num, frame_rate.den);
        hardware_eligible_ = input->duration > 0 && video_params->height >= 360 &&
            (frame_rate.num <= 0 || av_q2d(frame_rate) <= 30.01) &&
            (video_params->format == AV_PIX_FMT_YUV420P || video_params->format == AV_PIX_FMT_YUVJ420P) &&
            (profile == AV_PROFILE_H264_BASELINE || profile == AV_PROFILE_H264_CONSTRAINED_BASELINE ||
             profile == AV_PROFILE_H264_MAIN || profile == AV_PROFILE_H264_HIGH);
        const AVCodecParameters *audio_params = audio >= 0 ? input->streams[audio]->codecpar : nullptr;
        audio_ = audio_params && audio_params->codec_id == AV_CODEC_ID_AAC &&
                 (audio_params->extradata_size == 0 ||
                  (audio_params->extradata && audio_params->extradata_size >= 2 &&
                   audio_params->extradata_size <= 1024));
        audio_config_.clear();
        starfish_stream_.fps_num = frame_rate.num > 0 ? frame_rate.num : 30;
        starfish_stream_.fps_den = frame_rate.den > 0 ? frame_rate.den : 1;
        starfish_stream_.audio_rate = audio_ ? audio_params->sample_rate : 0;
        starfish_stream_.audio_channels = audio_ ? audio_params->ch_layout.nb_channels : 0;
        starfish_audio_supported_ = !audio_ || (audio_params->profile == AV_PROFILE_AAC_LOW &&
            starfish_stream_.audio_rate > 0 && starfish_stream_.audio_channels > 0 &&
            starfish_stream_.audio_channels <= 2);
        if (audio_ && !starfish_audio_supported_) {
            fprintf(stderr, "[direct] Starfish AAC unsupported: profile=%d rate=%d channels=%d\n",
                    audio_params->profile, starfish_stream_.audio_rate, starfish_stream_.audio_channels);
        }
        if (audio_ && audio_params->extradata_size > 0) {
            audio_config_.assign(audio_params->extradata,
                                 audio_params->extradata + audio_params->extradata_size);
        } else if (audio >= 0 && !audio_) {
            fprintf(stderr, "[direct] unsupported audio track\n");
        }

        const AVBitStreamFilter *annexb = av_bsf_get_by_name("h264_mp4toannexb");
        AVBSFContext *filter = nullptr;
        if (!annexb || av_bsf_alloc(annexb, &filter) < 0) return false;
        std::unique_ptr<AVBSFContext, decltype(&free_filter)> conversion(filter, free_filter);
        if (avcodec_parameters_copy(filter->par_in, video_params) < 0) return false;
        filter->time_base_in = input->streams[video]->time_base;
        if (av_bsf_init(filter) < 0) return false;

        const int64_t input_start = input->start_time == AV_NOPTS_VALUE ? 0 : input->start_time;
        hls_input_ = std::strcmp(input->iformat->name, "hls") == 0;
        if (needs_seek && input->duration > 0) {
            const auto seek_started = now_ns();
            deadline_ = seek_started + timeout_ns;
            const int64_t target = av_rescale_q(input_start + int64_t(start * AV_TIME_BASE), AV_TIME_BASE_Q,
                                                input->streams[video]->time_base);
            if ((!forward_keyframe || av_seek_frame(input, video, target, 0) < 0) &&
                av_seek_frame(input, video, target, AVSEEK_FLAG_BACKWARD) < 0)
                return false;
            if (hls_input_)
                fprintf(stderr, "[direct] HLS demux seek ready after %.3f s\n", (now_ns() - seek_started) / 1e9);
        }
        std::unique_ptr<AVPacket, decltype(&free_packet)> packet(av_packet_alloc(), free_packet);
        if (!packet) return false;
        width_ = video_params->width;
        height_ = video_params->height;
        duration_ms_ = input->duration > 0 ? int(input->duration / 1000) : 0;
        ready_ = true;

        auto save = [&](AVPacket &item, bool is_video, AVRational time_base) {
            if (item.size <= 0 || size_t(item.size) > max_packet_size) return false;
            const int64_t timestamp = is_video && item.dts != AV_NOPTS_VALUE ? item.dts : item.pts;
            if (timestamp == AV_NOPTS_VALUE) return true;
            const AVRational nanoseconds{1, int(ns_per_second)};
            const int64_t origin = input_start * 1000;
            const int64_t decode_at = av_rescale_q(timestamp, time_base, nanoseconds) - origin;
            const int64_t pts = item.pts == AV_NOPTS_VALUE ?
                decode_at : av_rescale_q(item.pts, time_base, nanoseconds) - origin;
            if (!is_video && pts < 0) return true;
            return enqueue({std::vector<uint8_t>(item.data, item.data + item.size), decode_at, pts, is_video});
        };
        while (!interrupted_) {
            deadline_ = now_ns() + timeout_ns;
            const int result = av_read_frame(input, packet.get());
            const bool timed_out = now_ns() >= deadline_;
            if (result == AVERROR_EOF && !timed_out) return true;
            if (result < 0 || (packet->flags & AV_PKT_FLAG_CORRUPT)) {
                read_error_ = !interrupted_;
                return false;
            }
            if (packet->stream_index == video) {
                if (av_bsf_send_packet(filter, packet.get()) < 0) return false;
                int filtered;
                while ((filtered = av_bsf_receive_packet(filter, packet.get())) == 0) {
                    if (!save(*packet, true, filter->time_base_out)) return interrupted_.load();
                    av_packet_unref(packet.get());
                }
                if (filtered != AVERROR(EAGAIN) && filtered != AVERROR_EOF) return false;
            } else if (packet->stream_index == audio && audio_) {
                if (!save(*packet, false, input->streams[audio]->time_base)) return interrupted_.load();
            }
            av_packet_unref(packet.get());
        }
        return true;
    };

    bool success = false;
    for (int attempt = 0; attempt < 3 && !interrupted_; ++attempt) {
        success = demux();
        if (success) break;
        close_input();
        if (ready_) break;
    }
    if (!success && !interrupted_) fprintf(stderr, "[direct] media stream failed\n");
    failed_ = !success && !interrupted_;
    finished_ = true;
}

void DirectClip::recover_hardware(const Media &media, bool starfish, const char *reason) {
    std::lock_guard<std::mutex> lock(requests_);
    if (!authorized_clip_ || start_requested_ || stop_requested_) return;
    const int64_t displayed = media.display_position();
    if (displayed >= 0) requested_position_ = double(displayed) / ns_per_second;
    else if (position_ms_ >= 0) requested_position_ = position_ms_ / 1000.0;
    if (starfish) {
        if (++starfish_retries_ < 3) {
            fprintf(stderr, "[direct] Starfish failed, attempting retry %u/2 before LGNC fallback\n", starfish_retries_);
            if (starfish_retries_ == 1 && media.starfish_end_error() && displayed >= 0)
                starfish_fault_position_ = requested_position_;
            if (starfish_retries_ == 2 && media.starfish_end_error() && starfish_fault_position_ >= 0 &&
                starfish_fault_position_ + 2 < duration_ms_ / 1000.0) {
                requested_position_ = starfish_fault_position_ + 0.5;
                forward_keyframe_ = true;
            }
        } else {
            starfish_failed_ = true;
            if (starfish_fault_position_ >= 0 &&
                (displayed < 0 || requested_position_ <= starfish_fault_position_ + 2))
                requested_position_ = starfish_fault_position_;
            fprintf(stderr, "[direct] Starfish exhausted retries, falling back to LGNC\n");
        }
    } else {
        hardware_failed_ = true;
    }
    reopen_input_ = start_requested_ = true;
    fprintf(stderr, "[direct] recovering from %s at %.2f s (%s%s)\n",
            starfish ? "Starfish" : "LGNC", requested_position_, reason,
            forward_keyframe_ ? ", next keyframe" : "");
}

FeedResult DirectClip::feed_sample(Media &media, const Sample &sample) {
    const auto result = media.feed(sample.data.data(), sample.data.size(), sample.pts, sample.video);
    if (result != FeedResult::Retry) {
        hardware_retry_since_ = 0;
        return result;
    }
    const int64_t now = now_ns();
    if (hardware_retry_since_ == 0) hardware_retry_since_ = now;
    const int64_t limit = media.starfish_video() ? 2500 : 750;
    return now - hardware_retry_since_ < limit * ns_per_ms ? FeedResult::Retry : FeedResult::Error;
}

bool DirectClip::tick(Media &media, AudioDecoder &decoder, Shell &shell, const char *app_id,
                      const std::function<void()> &before_start,
                      const std::function<void(const char *)> &show_status) {
    if (pointer_seconds_ >= 0 && now_ns() >= pointer_until_) {
        pointer_seconds_ = -1;
        media.redraw();
    }
    if (scrub_seconds_ >= 0 && scrub_commit_at_ && now_ns() >= scrub_commit_at_) {
        const int target = scrub_seconds_;
        scrub_seconds_ = -1;
        scrub_commit_at_ = 0;
        fprintf(stderr, "[remote] quick seek to %d seconds\n", target);
        seek(target);
    }
    bool start, stop_requested, pause, new_clip, reopen, forward_keyframe;
    double seek;
    std::string source;
    {
        std::lock_guard<std::mutex> lock(requests_);
        start = start_requested_;
        stop_requested = stop_requested_;
        pause = requested_pause_ || (scrub_seconds_ >= 0 && !loading_);
        new_clip = new_clip_;
        reopen = reopen_input_;
        forward_keyframe = forward_keyframe_;
        if (start) source = requested_source_;
        seek = requested_position_;
        start_requested_ = stop_requested_ = new_clip_ = reopen_input_ = false;
        forward_keyframe_ = false;
    }
    if (start || (stop_requested && (playing_ || worker_.joinable()))) {
        if (playing_) {
            fprintf(stderr, "[direct] ended packets=%u frames=%u audio=%u\n",
                    feeds_.load(), media.frames.load(), audio_feeds_.load());
        }
        if (stop_requested) {
            loading_ = false;
            media.end_loading();
        } else {
            loading_ = true;
            loading_started_ = now_ns();
            loading_target_ = int(seek);
            if (duration_ms_ > 0) loading_duration_ = duration_ms_.load() / 1000;
            const bool keep_picture = playing_ || worker_.joinable();
            media.begin_loading(keep_picture);
            if (keep_picture) fprintf(stderr, "[direct] seek loader started\n");
            next_loader_draw_ = 0;
        }
        const bool reuse_input = !stop_requested && !new_clip && !reopen;
        join(reuse_input);
        quality_index_ = -1;
        pointer_seconds_ = -1;
        if (stop_requested || new_clip) {
            hardware_failed_ = false;
            starfish_failed_ = false;
            starfish_retries_ = 0;
            starfish_fault_position_ = -1;
            next_quality_height_ = 0;
            std::lock_guard<std::mutex> lock(requests_);
            rendition_heights_.clear();
            multiple_renditions_ = false;
            network_retries_ = 0;
            scrub_seconds_ = -1;
            scrub_commit_at_ = 0;
        }
        quality_height_ = next_quality_height_;
        prefer_hardware_hls_ = !hardware_failed_ &&
            (media.hardware_enabled() || (media.starfish_enabled() && !starfish_failed_));
        if (!reuse_input) promoted_hls_ = false;
        media.close();
        decoder.reset();
        playing_ = paused_ = false;
        rebuffering_ = false;
        empty_since_ = 0;
        hardware_retry_since_ = 0;
        next_hardware_draw_ = 0;
        hardware_drain_until_ = 0;
        starfish_draining_ = false;
        starfish_eos_sent_ = false;
        starfish_tail_completed_ = false;
        last_video_pts_ = -1;
        hardware_overlay_visible_ = false;
        last_hardware_feed_ = max_hardware_gap_ = max_hardware_lateness_ = 0;
        if (stop_requested) return false;
        preview_at_ = seek > 0 ? int64_t(seek * ns_per_second) : (pause ? 0 : -1);
        controls_until_ = now_ns() + 4 * ns_per_second;
        before_start();
        interrupted_ = false;
        finished_ = failed_ = read_error_ = false;
        feeds_ = audio_feeds_ = 0;
        worker_ = std::thread(&DirectClip::read, this, seek, std::move(source), forward_keyframe);
    }

    if (!worker_.joinable()) return false;
    if (loading_ && now_ns() >= next_loader_draw_) {
        media.redraw();
        next_loader_draw_ = now_ns() + 80000000;
    }
    if (!playing_ && ready_) {
        const bool audio_ready = audio_ && decoder.configure_aac(audio_config_.data(), audio_config_.size());
        const bool attempt_starfish = media.starfish_enabled() && !starfish_failed_ && starfish_audio_supported_;
        const bool attempt_hardware = (media.hardware_enabled() || attempt_starfish) &&
            hardware_eligible_ && !hardware_failed_;
        if (media.open_clip(width_, height_, audio_ready, attempt_hardware ? app_id : nullptr,
                            attempt_starfish ? &starfish_stream_ : nullptr)) {
            if (attempt_hardware && !media.hardware_video()) {
                hardware_failed_ = true;
                fprintf(stderr, "[direct] LGNC unavailable, using software\n");
            }
            if (promoted_hls_ && !media.hardware_video()) {
                std::lock_guard<std::mutex> lock(requests_);
                if (authorized_clip_ && !start_requested_ && !stop_requested_) {
                    hardware_failed_ = true;
                    reopen_input_ = start_requested_ = true;
                    fprintf(stderr, "[direct] retrying Auto HLS with a software rendition\n");
                }
                return true;
            }
            if (preview_at_ >= 0) media.show_frames_from(preview_at_);
            playing_ = true;
            origin_ = 0;
            fprintf(stderr, "[direct] video open %dx%d audio=%d\n", width_.load(), height_.load(), int(audio_));
        } else if (attempt_hardware && attempt_starfish) {
            recover_hardware(media, true, "pipeline open failed");
            return true;
        } else {
            stop();
        }
    }
    if (media.starfish_video()) {
        media.tick();
        if (media.failed) {
            const int64_t displayed = media.display_position();
            bool input_drained = false;
            if (finished_ && !failed_) {
                std::lock_guard<std::mutex> lock(mutex_);
                input_drained = queue_.empty();
            }
            // Rockhopper can report an error after EOS even when the final
            // presentation interval has already completed.
            const bool drained = input_drained && media.starfish_end_error() &&
                displayed >= 0 && last_video_pts_ >= 0 &&
                displayed + 900 * ns_per_ms >= last_video_pts_ &&
                last_video_pts_ / ns_per_ms + 500 >= duration_ms_;
            if (!drained) {
                recover_hardware(media, true, "pipeline reported an error");
                return true;
            }
            if (!starfish_tail_completed_) {
                starfish_tail_completed_ = true;
                fprintf(stderr, "[direct] Starfish ended after all packets were submitted\n");
            }
        }
    }
    if (media.hardware_video() && !loading_) {
        const int64_t now = now_ns();
        const bool show_overlay = paused_ || quality_index_ >= 0 || scrub_seconds_ >= 0 || now < controls_until_;
        if ((show_overlay && now >= next_hardware_draw_) || (hardware_overlay_visible_ && !show_overlay)) {
            media.redraw();
            next_hardware_draw_ = now + 250 * ns_per_ms;
            hardware_overlay_visible_ = show_overlay;
        }
    }
    {
        const int64_t now = now_ns();
        if (playing_ && paused_ != pause) {
            empty_since_ = 0;
            last_hardware_feed_ = 0;
            if (!media.starfish_video() || preview_at_ < 0) media.pause(pause || rebuffering_);
            if (pause) paused_at_ = now;
            else if (origin_ != 0) origin_ += now - paused_at_;
            paused_ = pause;
            media.redraw();
        }
    }
    if (playing_ && !paused_ && preview_at_ < 0 && origin_ != 0 && !finished_) {
        const int64_t now = now_ns();
        bool start_buffering = false, resume = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                if (empty_since_ == 0) empty_since_ = now;
                start_buffering = !rebuffering_ && now - empty_since_ >= rebuffer_delay;
            } else {
                empty_since_ = 0;
                const bool enough = queue_.back().decode_at - queue_.front().decode_at >= resume_buffer ||
                    queue_.size() == max_queue_packets || queued_bytes_ > max_queue_bytes - max_packet_size;
                resume = rebuffering_ && enough;
                if (resume) origin_ = now - queue_.front().decode_at;
            }
        }
        if (start_buffering) {
            last_hardware_feed_ = 0;
            rebuffering_ = loading_ = true;
            loading_target_ = -1;
            media.pause(true);
            media.begin_loading(true);
            fprintf(stderr, "[direct] buffering\n");
        } else if (resume) {
            rebuffering_ = loading_ = false;
            media.end_loading();
            media.pause(false);
            fprintf(stderr, "[direct] resumed\n");
        }
    }
    if (rebuffering_ && finished_ && !paused_) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!queue_.empty()) origin_ = now_ns() - queue_.front().decode_at;
        }
        rebuffering_ = loading_ = false;
        media.end_loading();
        media.pause(false);
    }
    bool empty = false;
    for (int budget = 0; budget < 6; ++budget) {
        Sample sample;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            empty = queue_.empty();
            if (empty || !playing_ || rebuffering_ || (paused_ && preview_at_ < 0)) break;
            const auto now = now_ns();
            // HLS seeks to the segment's keyframe. Feed preroll without
            // waiting in real time, then pace normally at the target.
            const bool hls_preroll = preview_at_ >= 0 && hls_input_ &&
                queue_.front().decode_at < preview_at_;
            if (preview_at_ < 0 || (media.starfish_video() && !hls_preroll)) {
                if (origin_ == 0) origin_ = now - queue_.front().decode_at;
                if (queue_.front().decode_at > now - origin_) break;
            }
            queued_bytes_ -= queue_.front().data.size();
            sample = std::move(queue_.front());
            queue_.pop_front();
        }
        const bool starfish_audio = media.starfish_video() && !sample.video &&
            (preview_at_ < 0 || sample.pts >= preview_at_);
        const int64_t submit_at = now_ns();
        if (sample.video && preview_at_ < 0) media.set_playback_clock(submit_at - origin_);
        if (sample.video || starfish_audio) {
            const auto result = feed_sample(media, sample);
            if (result == FeedResult::Retry) {
                std::lock_guard<std::mutex> lock(mutex_);
                queued_bytes_ += sample.data.size();
                queue_.push_front(std::move(sample));
                break;
            }
            if (result == FeedResult::Error) {
                if (media.hardware_video()) {
                    recover_hardware(media, media.starfish_video(), "packet submission failed");
                    return true;
                }
                stop();
                break;
            }
            if (starfish_audio) ++audio_feeds_;
        }
        if (sample.video) {
            last_video_pts_ = std::max(last_video_pts_, sample.pts);
            if (media.hardware_video() && preview_at_ < 0) {
                if (last_hardware_feed_ != 0)
                    max_hardware_gap_ = std::max(max_hardware_gap_, submit_at - last_hardware_feed_);
                last_hardware_feed_ = submit_at;
                max_hardware_lateness_ = std::max(max_hardware_lateness_, submit_at - origin_ - sample.decode_at);
            }
            const unsigned count = ++feeds_;
            if (count == 1 || count % 150 == 0) {
                fprintf(stderr, "[direct] packets=%u frames=%u audio=%u\n",
                        count, media.frames.load(), audio_feeds_.load());
                if (media.hardware_video() && count > 1) {
                    fprintf(stderr, "[direct] hardware max feed gap=%lld ms late=%lld ms\n",
                            static_cast<long long>(max_hardware_gap_ / ns_per_ms),
                            static_cast<long long>(max_hardware_lateness_ / ns_per_ms));
                    max_hardware_gap_ = max_hardware_lateness_ = 0;
                }
            }
        } else if (!media.starfish_video() && preview_at_ < 0 &&
                   sample.pts >= now_ns() - origin_ - 160 * ns_per_ms) {
            std::vector<uint8_t> pcm;
            if (decoder.decode(4, sample.data.data(), sample.data.size(), pcm) && !pcm.empty()) {
                if (media.feed(pcm.data(), pcm.size(), sample.pts, false) == FeedResult::Accepted) ++audio_feeds_;
            }
        }
        available_.notify_one();
        position_ms_ = int(std::max(int64_t(0), sample.decode_at) / ns_per_ms);
        if (sample.video && preview_at_ >= 0 && media.frames > 0) {
            preview_at_ = -1;
            // Anchor playback to the prefetched frame after a seek.
            if (!media.starfish_video()) origin_ = now_ns() - sample.decode_at;
            if (paused_) {
                paused_at_ = now_ns();
                media.pause(true);
            }
        }
        if (loading_ && preview_at_ < 0 && media.frames > 0) {
            loading_ = false;
            media.end_loading();
            media.redraw();
            fprintf(stderr, "[direct] first frame ready after %.2f s\n", (now_ns() - loading_started_) / 1e9);
        }
    }
    if (finished_ && empty) {
        if (media.starfish_video()) {
            const int64_t now = now_ns();
            if (!starfish_draining_) {
                starfish_draining_ = true;
                hardware_drain_until_ = now + 1200 * ns_per_ms;
            }
            if (!starfish_eos_sent_ && !starfish_tail_completed_) {
                const int64_t displayed = media.display_position();
                if (displayed < last_video_pts_ - 200 * ns_per_ms && now < hardware_drain_until_) return true;
                media.eos();
                starfish_eos_sent_ = true;
                hardware_drain_until_ = now + 1000 * ns_per_ms;
            }
            if (!media.ended && !media.failed && now < hardware_drain_until_) return true;
        } else if (media.hardware_video() && media.frames > 0) {
            const int64_t now = now_ns();
            if (hardware_drain_until_ == 0) hardware_drain_until_ = now + 180 * ns_per_ms;
            if (now < hardware_drain_until_) return true;
        }
        const int position = position_ms_;
        media.end_loading();
        if (!starfish_draining_) media.eos();
        const bool no_picture = media.frames == 0;
        // Some HTTP failures look like clean EOF to the demuxer.
        constexpr int64_t end_tolerance_ms = 2000;
        const bool ended_early = position >= 0 && position + end_tolerance_ms < duration_ms_;
        if ((no_picture || ended_early || (failed_ && read_error_)) && network_retries_ < 2) {
            std::lock_guard<std::mutex> lock(requests_);
            if (authorized_clip_ && !start_requested_ && !stop_requested_) {
                if (promoted_hls_ && no_picture) hardware_failed_ = true;
                if (!fallback_source_.empty() && no_picture) {
                    requested_source_ = std::move(fallback_source_);
                    fallback_source_.clear();
                    fprintf(stderr, "[direct] mapped MP4 had no video, trying original HLS\n");
                }
                if (!no_picture && position > 0) requested_position_ = position / 1000.0;
                reopen_input_ = start_requested_ = true;
                ++network_retries_;
                fprintf(stderr, "[direct] reconnecting at %.2f s (attempt %u)\n",
                        requested_position_, network_retries_);
                return true;
            }
        }
        fprintf(stderr, "[direct] done packets=%u frames=%u audio=%u\n",
                feeds_.load(), media.frames.load(), audio_feeds_.load());
        const bool playback_failed = failed_ || (media.failed && !starfish_tail_completed_);
        join();
        quality_index_ = -1;
        loading_ = false;
        media.close();
        decoder.reset();
        playing_ = false;
        const char *message = no_picture ? "Safari source returned no video" :
            (playback_failed || ended_early) ? "Safari video interrupted" : "Safari video finished";
        show_status(message);
        shell.screen(message, "");
    }
    return true;
}

void DirectClip::info(playback_info_t &out) const {
    out.duration = double(duration_ms_.load()) / 1000;
    out.position = double(position_ms_.load()) / 1000;
    if (out.duration < 0) out.duration = 0;
    if (out.position < 0) out.position = -1;
    out.seek_start = 0;
    out.seek_duration = out.duration;
    {
        std::lock_guard<std::mutex> lock(requests_);
        out.rate = requested_pause_ ? 0 : 1;
    }
    out.ready_to_play = ready_;
    out.playback_buffer_empty = !ready_ || rebuffering_;
    out.playback_buffer_full = false;
    out.playback_likely_to_keep_up = ready_ && !rebuffering_;
}
