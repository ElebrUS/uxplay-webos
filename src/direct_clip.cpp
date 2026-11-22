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
#include <limits>
#include <fstream>
#include <memory>

namespace {
constexpr int64_t ns_per_second = 1000000000;
constexpr int64_t ns_per_ms = 1000000;
constexpr int64_t timeout_ns = 5 * ns_per_second;
constexpr size_t max_packet_size = 1024 * 1024;

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
    new_clip_ = true;
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
    start_requested_ = true;
    stop_requested_ = false;
}
void DirectClip::pause(bool value) {
    std::lock_guard<std::mutex> lock(requests_);
    requested_pause_ = value;
}
bool DirectClip::remote(Action action, Media &media) {
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

void DirectClip::draw_controls(Shell &shell) const {
    if (loading_) shell.playback_loader();
    if (scrub_seconds_ >= 0) {
        const int duration = duration_ms_ > 0 ? duration_ms_.load() / 1000 : loading_duration_;
        shell.playback_controls(position_ms_ / 1000, duration, true, scrub_seconds_, !scrub_commit_at_);
    } else if (loading_ && loading_target_ >= 0 && loading_duration_ > 0) {
        shell.playback_controls(loading_target_, loading_duration_, false);
    } else if (playing_ && !loading_ && (paused_ || now_ns() < controls_until_)) {
        shell.playback_controls(position_ms_ / 1000, duration_ms_ / 1000, paused_);
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
    const auto filtered = select_hls_rendition(playlist_text_);
    if (filtered != playlist_text_) fprintf(stderr, "[direct] selected one H.264 variant before opening segments\n");
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
    available_.wait(lock, [&] { return interrupted_ || queue_.size() < 64; });
    if (interrupted_) return false;
    queue_.push_back(std::move(sample));
    return true;
}

void DirectClip::read(double start, std::string source) {
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
        const AVCodecParameters *audio_params = audio >= 0 ? input->streams[audio]->codecpar : nullptr;
        audio_ = audio_params && audio_params->codec_id == AV_CODEC_ID_AAC &&
                 (audio_params->extradata_size == 0 ||
                  (audio_params->extradata && audio_params->extradata_size >= 2 &&
                   audio_params->extradata_size <= 1024));
        audio_config_.clear();
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
        if (needs_seek && input->duration > 0) {
            deadline_ = now_ns() + timeout_ns;
            const int64_t target = av_rescale_q(input_start + int64_t(start * AV_TIME_BASE), AV_TIME_BASE_Q,
                                                input->streams[video]->time_base);
            if (av_seek_frame(input, video, target, AVSEEK_FLAG_BACKWARD) < 0) return false;
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
            if (result == AVERROR_EOF) return true;
            if (result < 0) return false;
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

bool DirectClip::tick(Media &media, AudioDecoder &decoder, Shell &shell, const std::function<void()> &before_start,
                      const std::function<void(const char *)> &show_status) {
    if (scrub_seconds_ >= 0 && scrub_commit_at_ && now_ns() >= scrub_commit_at_) {
        const int target = scrub_seconds_;
        scrub_seconds_ = -1;
        scrub_commit_at_ = 0;
        fprintf(stderr, "[remote] quick seek to %d seconds\n", target);
        seek(target);
    }
    bool start, stop_requested, pause, new_clip;
    double seek;
    std::string source;
    {
        std::lock_guard<std::mutex> lock(requests_);
        start = start_requested_;
        stop_requested = stop_requested_;
        pause = requested_pause_ || (scrub_seconds_ >= 0 && !loading_);
        new_clip = new_clip_;
        if (start) source = requested_source_;
        seek = requested_position_;
        start_requested_ = stop_requested_ = new_clip_ = false;
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
        join(!stop_requested && !new_clip);
        if (stop_requested || new_clip) {
            scrub_seconds_ = -1;
            scrub_commit_at_ = 0;
        }
        media.close();
        decoder.reset();
        playing_ = paused_ = false;
        if (stop_requested) return false;
        preview_at_ = seek > 0 ? int64_t(seek * ns_per_second) : (pause ? 0 : -1);
        controls_until_ = now_ns() + 4 * ns_per_second;
        before_start();
        interrupted_ = false;
        finished_ = failed_ = false;
        feeds_ = audio_feeds_ = 0;
        worker_ = std::thread(&DirectClip::read, this, seek, std::move(source));
    }

    if (!worker_.joinable()) return false;
    if (loading_ && now_ns() >= next_loader_draw_) {
        media.redraw();
        next_loader_draw_ = now_ns() + 80000000;
    }
    if (!playing_ && ready_) {
        const bool audio_ready = audio_ && decoder.configure_aac(audio_config_.data(), audio_config_.size());
        if (media.open_clip(width_, height_, audio_ready)) {
            if (preview_at_ >= 0) media.show_frames_from(preview_at_);
            playing_ = true;
            origin_ = 0;
            fprintf(stderr, "[direct] video open %dx%d audio=%d\n", width_.load(), height_.load(), int(audio_));
        } else {
            stop();
        }
    }
    {
        const int64_t now = now_ns();
        if (playing_ && paused_ != pause) {
            media.pause(pause);
            if (pause) paused_at_ = now;
            else if (origin_ != 0) origin_ += now - paused_at_;
            paused_ = pause;
            media.redraw();
        }
    }
    bool empty = false;
    for (int budget = 0; budget < 6; ++budget) {
        Sample sample;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            empty = queue_.empty();
            if (empty || !playing_ || (paused_ && preview_at_ < 0)) break;
            const auto now = now_ns();
            if (preview_at_ < 0) {
                if (origin_ == 0) origin_ = now - queue_.front().decode_at;
                if (queue_.front().decode_at > now - origin_) break;
            }
            sample = std::move(queue_.front());
            queue_.pop_front();
        }
        available_.notify_one();
        if (sample.video) {
            if (media.feed(sample.data.data(), sample.data.size(), sample.pts, true) ==
                FeedResult::Error) {
                stop();
                break;
            }
            const unsigned count = ++feeds_;
            if (count == 1 || count % 150 == 0) {
                fprintf(stderr, "[direct] packets=%u frames=%u audio=%u\n",
                        count, media.frames.load(), audio_feeds_.load());
            }
        } else if (preview_at_ < 0) {
            std::vector<uint8_t> pcm;
            if (decoder.decode(4, sample.data.data(), sample.data.size(), pcm) && !pcm.empty()) {
                if (media.feed(pcm.data(), pcm.size(), sample.pts, false) == FeedResult::Accepted) ++audio_feeds_;
            }
        }
        position_ms_ = int(std::max(int64_t(0), sample.decode_at) / ns_per_ms);
        if (sample.video && preview_at_ >= 0 && media.frames > 0) {
            preview_at_ = -1;
        }
        if (loading_ && preview_at_ < 0 && media.frames > 0) {
            loading_ = false;
            media.end_loading();
            media.redraw();
            fprintf(stderr, "[direct] first frame ready after %.2f s\n", (now_ns() - loading_started_) / 1e9);
        }
    }
    if (finished_ && empty) {
        media.end_loading();
        media.eos();
        fprintf(stderr, "[direct] done packets=%u frames=%u audio=%u\n",
                feeds_.load(), media.frames.load(), audio_feeds_.load());
        join();
        loading_ = false;
        media.close();
        decoder.reset();
        playing_ = false;
        const char *message = failed_ ? "Could not play this stream" : "Safari video finished";
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
    out.playback_buffer_empty = !ready_;
    out.playback_buffer_full = false;
    out.playback_likely_to_keep_up = ready_;
}
