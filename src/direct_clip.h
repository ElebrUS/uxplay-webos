#pragma once

#include "raop.h"
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
class AudioDecoder;
class Media;
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
    bool tick(Media &media, AudioDecoder &decoder, Shell &shell, const std::function<void()> &before_start,
              const std::function<void(const char *)> &show_status);
    bool remote(Action action, Media &media);
    void draw_controls(Shell &shell) const;
    void info(playback_info_t &out) const;

private:
    struct Sample {
        std::vector<uint8_t> data;
        int64_t decode_at;
        int64_t pts;
        bool video;
    };
    void read(double start, std::string source);
    void join(bool keep_input = false);
    void close_input();
    bool load_playlist(const std::string &url, std::string &resolved);
    bool enqueue(Sample sample);
    std::string profile_url_;
    std::string profile_media_;
    std::string requested_source_;
    std::vector<uint8_t> audio_config_;
    std::thread worker_;
    AVFormatContext *input_ = nullptr;
    AVIOContext *playlist_io_ = nullptr;
    std::string playlist_text_;
    size_t playlist_offset_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<Sample> queue_;
    std::atomic<bool> interrupted_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> ready_{false};
    std::atomic<int64_t> deadline_{0};
    mutable std::mutex requests_;
    bool authorized_clip_ = false;
    bool start_requested_ = false;
    bool new_clip_ = false;
    bool stop_requested_ = false;
    bool requested_pause_ = false;
    bool playing_ = false;
    bool paused_ = false;
    double requested_position_ = 0;
    bool loading_ = false;
    int64_t next_loader_draw_ = 0;
    int64_t controls_until_ = 0;
    int scrub_seconds_ = -1;
    int64_t scrub_commit_at_ = 0;
    int64_t hold_started_ = 0;
    int64_t last_scrub_step_ = 0;
    int64_t loading_started_ = 0;
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
    std::atomic<unsigned> feeds_{0};
    std::atomic<unsigned> audio_feeds_{0};
};
