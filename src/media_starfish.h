#pragma once

#include "media.h"
#include <memory>

class StarfishVideo {
public:
    StarfishVideo();
    ~StarfishVideo();

    bool open(const char *app_id, int width, int height, const StarfishStream &stream);
    void close();
    bool opened() const;
    bool failed() const;
    bool rockhopper_end_error() const;
    bool ended() const;
    unsigned frames() const;
    int64_t position() const;
    FeedResult feed(const void *data, size_t size, int64_t pts, bool video);
    void tick();
    void pause(bool paused);
    void eos();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
