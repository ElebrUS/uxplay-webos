#pragma once
#include <atomic>
#ifdef WEBOS14
#include <functional>
#endif
#include <cstdint>
#include <cstddef>
#include <string>
class StarfishMediaAPIs;
struct SDL_Renderer;
enum class FeedResult { Accepted, Retry, Error };
class Media {
public:
#ifdef WEBOS14
    explicit Media(SDL_Renderer *renderer);
    bool open_clip(int width, int height, bool audio);
    void show_frames_from(int64_t pts_ns);
    void set_playback_clock(int64_t pts_ns);
    void set_overlay(std::function<void()> overlay);
    void redraw();
    void begin_loading(bool preserve_frame);
    void end_loading();
#else
    Media();
#endif
    Media(const Media &) = delete;
    Media &operator=(const Media &) = delete;
    ~Media();
    bool open(const char *app_id, int width, int height, bool audio, int sample_rate = 44100);
    FeedResult feed(const void *data, size_t size, int64_t pts_ns, bool video, bool present_video = true);
    void tick();
    void eos();
    void pause(bool paused);
    void close();
    std::atomic<bool> ready{false}, playing{false}, ended{false}, failed{false};
    std::atomic<unsigned> frames{0};
private:
#ifdef WEBOS14
    bool open_decoder(int width, int height, bool audio, int sample_rate, bool buffered);
    FeedResult receive_frames(bool present_video);
#endif
#ifndef WEBOS14
    static void event(int type, int64_t value, const char *message, void *user);
    StarfishMediaAPIs *api_ = nullptr;
    long acb_ = 0, task_ = 0;
    bool loaded_ = false, sink_attached_ = false, play_requested_ = false, acb_playing_ = false;
    std::atomic<bool> video_info_pending_{false};
#endif
    struct Impl;
    Impl *impl_;
};
