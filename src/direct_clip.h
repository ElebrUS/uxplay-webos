#pragma once

#include "raop.h"
#include "media.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVIOContext;
struct SDL_Point;
class AudioDecoder;
class Shell;
enum class Action;

class DirectClip {
public:
    DirectClip();
    ~DirectClip();
    bool play(const char *url, double start);
    void seek(double position);
    void pause(bool paused);
    void stop();
    bool tick(Media &media, AudioDecoder &decoder, Shell &shell, const char *app_id,
              const std::function<void()> &before_start,
              const std::function<void(const char *)> &show_status);
    bool remote(Action action, Media &media, SDL_Point pointer);
    void draw_controls(Shell &shell, const Media &media) const;
    void info(playback_info_t &out) const;

private:
    struct Sample {
        std::vector<uint8_t> data;
        int64_t decode_at;
        int64_t pts;
        bool video;
    };
    void read(double start, std::string source, bool forward_keyframe);
    void join(bool keep_input = false);
    void close_input();
    bool load_playlist(const std::string &url, std::string &resolved);
    bool quality_remote(Action action);
    bool retry_input(bool no_picture, int position);
    void recover_hardware(const Media &media, bool starfish, const char *reason);
    FeedResult feed_sample(Media &media, const Sample &sample);
    void feed_split_audio(Media &media, AudioDecoder &decoder);
    bool enqueue(Sample sample);
    std::string profile_url_;
    std::string profile_media_;
    std::string requested_source_;
    std::string fallback_source_;
    std::vector<int> rendition_heights_;
    std::atomic<bool> multiple_renditions_{false};
    std::vector<int> quality_choices_;
    int quality_index_ = -1;
    int quality_height_ = 0;
    int next_quality_height_ = 0;
    std::vector<uint8_t> audio_config_;
    std::thread worker_;
    AVFormatContext *input_ = nullptr;
    AVIOContext *playlist_io_ = nullptr;
    std::string playlist_text_;
    size_t playlist_offset_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<Sample> queue_;
    std::deque<Sample> split_audio_queue_;
    int64_t split_video_position_ = -1;
    int64_t split_video_observed_at_ = 0;
    size_t queued_bytes_ = 0;
    std::atomic<bool> interrupted_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> read_error_{false};
    std::atomic<int> input_error_{0};
    std::atomic<bool> ready_{false};
    std::atomic<int64_t> deadline_{0};
    mutable std::mutex requests_;
    bool authorized_clip_ = false;
    bool start_requested_ = false;
    bool new_clip_ = false;
    bool reopen_input_ = false;
    bool stop_requested_ = false;
    bool requested_pause_ = false;
    bool playing_ = false;
    bool paused_ = false;
    bool hardware_failed_ = false;
    bool starfish_failed_ = false;
    unsigned starfish_retries_ = 0;
    double starfish_fault_position_ = -1;
    bool forward_keyframe_ = false;
    bool active_forward_keyframe_ = false;
    bool prefer_hardware_hls_ = false;
    std::atomic<bool> promoted_hls_{false};
    int64_t hardware_retry_since_ = 0;
    int64_t next_hardware_draw_ = 0;
    int64_t hardware_drain_until_ = 0;
    bool starfish_draining_ = false;
    bool starfish_eos_sent_ = false;
    bool starfish_tail_completed_ = false;
    int64_t last_video_pts_ = -1;
    bool hardware_overlay_visible_ = false;
    int64_t last_hardware_feed_ = 0;
    int64_t max_hardware_gap_ = 0;
    int64_t max_hardware_lateness_ = 0;
    double requested_position_ = 0;
    bool loading_ = false;
    std::atomic<bool> rebuffering_{false};
    int64_t empty_since_ = 0;
    int64_t buffering_started_ = 0;
    unsigned network_retries_ = 0;
    int64_t next_loader_draw_ = 0;
    int64_t controls_until_ = 0;
    int scrub_seconds_ = -1;
    int pointer_seconds_ = -1;
    int64_t pointer_until_ = 0;
    int64_t scrub_commit_at_ = 0;
    int64_t hold_started_ = 0;
    int64_t last_scrub_step_ = 0;
    int64_t loading_started_ = 0;
    std::atomic<int64_t> first_video_queued_at_{0};
    int64_t target_submitted_at_ = 0;
    int loading_target_ = -1;
    int loading_duration_ = 0;
    int64_t preview_at_ = -1;
    int64_t origin_ = 0;
    int64_t paused_at_ = 0;
    std::atomic<int> width_{0};
    std::atomic<int> height_{0};
    // 32-bit atomic millisecond values avoid a heavyweight runtime for queries.
    std::atomic<int> duration_ms_{-1};
    std::atomic<int> position_ms_{-1};
    std::atomic<bool> audio_{false};
    std::atomic<bool> hardware_eligible_{false};
    bool hls_input_ = false;
    StarfishStream starfish_stream_;
    bool starfish_audio_supported_ = false;
    std::atomic<unsigned> feeds_{0};
    std::atomic<unsigned> audio_feeds_{0};
};
