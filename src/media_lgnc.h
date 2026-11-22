#pragma once

#include "media.h"
#include <lgnc_directvideo.h>
#include <lgnc_plugin.h>
#include <vector>

class LgncVideo {
public:
    ~LgncVideo();
    bool open(const char *app_id, int width, int height);
    void close();
    FeedResult feed(const void *data, size_t size);
    bool opened() const { return opened_; }

private:
    bool initialize(const char *app_id);
    void *library_ = nullptr;
    bool attempted_ = false;
    bool initialized_ = false;
    bool opened_ = false;
    LGNC_CALLBACKS_T callbacks_{};
    decltype(&LGNC_PLUGIN_Initialize) initialize_ = nullptr;
    decltype(&LGNC_PLUGIN_SetAppId) set_app_id_ = nullptr;
    decltype(&LGNC_PLUGIN_Finalize) finalize_ = nullptr;
    decltype(&LGNC_DIRECTVIDEO_Open) open_ = nullptr;
    decltype(&LGNC_DIRECTVIDEO_Close) close_ = nullptr;
    decltype(&LGNC_DIRECTVIDEO_Play) play_ = nullptr;
    decltype(&_LGNC_DIRECTVIDEO_SetDisplayWindow) set_window_ = nullptr;
    std::vector<unsigned char> access_unit_;
};
