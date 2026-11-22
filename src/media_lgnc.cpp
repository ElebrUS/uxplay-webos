#include "media_lgnc.h"
#include <algorithm>
#include <cstdio>
#include <dlfcn.h>

namespace {
template <typename Function>
bool load(void *library, const char *name, Function &function) {
    function = reinterpret_cast<Function>(dlsym(library, name));
    if (!function) std::fprintf(stderr, "[lgnc] missing %s\n", name);
    return function != nullptr;
}
}

LgncVideo::~LgncVideo() {
    close();
    if (initialized_) finalize_();
    if (library_) dlclose(library_);
}

bool LgncVideo::initialize(const char *app_id) {
    if (attempted_) return initialized_;
    attempted_ = true;
    library_ = dlopen("liblgncopenapi.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!library_) {
        std::fprintf(stderr, "[lgnc] %s\n", dlerror());
        return false;
    }
    if (!load(library_, "LGNC_PLUGIN_Initialize", initialize_) ||
        !load(library_, "LGNC_PLUGIN_SetAppId", set_app_id_) ||
        !load(library_, "LGNC_PLUGIN_Finalize", finalize_) ||
        !load(library_, "LGNC_DIRECTVIDEO_Open", open_) ||
        !load(library_, "LGNC_DIRECTVIDEO_Close", close_) ||
        !load(library_, "LGNC_DIRECTVIDEO_Play", play_) ||
        !load(library_, "_LGNC_DIRECTVIDEO_SetDisplayWindow", set_window_)) return false;
    const int status = initialize_(&callbacks_);
    if (status != 0) {
        std::fprintf(stderr, "[lgnc] initialize failed: %d\n", status);
        return false;
    }
    if (set_app_id_(app_id) != 0) {
        std::fprintf(stderr, "[lgnc] application registration failed\n");
        finalize_();
        return false;
    }
    initialized_ = true;
    return true;
}

bool LgncVideo::open(const char *app_id, int width, int height) {
    close();
    if (!initialize(app_id)) return false;
    const LGNC_VDEC_DATA_INFO_T format{width, height, LGNC_VDEC_FMT_H264, LGNC_VDEC_3D_TYPE_NONE};
    const int status = open_(&format);
    if (status != 0) {
        std::fprintf(stderr, "[lgnc] open failed: %d\n", status);
        return false;
    }
    opened_ = true;
    const int fitted_width = std::min(1920, 1080 * width / height);
    const int fitted_height = std::min(1080, 1920 * height / width);
    if (set_window_((1920 - fitted_width) / 2, (1080 - fitted_height) / 2,
                    fitted_width, fitted_height) != 0) {
        std::fprintf(stderr, "[lgnc] could not set the video window\n");
        close();
        return false;
    }
    std::fprintf(stderr, "[lgnc] video open %dx%d\n", width, height);
    return true;
}

void LgncVideo::close() {
    if (!opened_) return;
    opened_ = false;
    const int status = close_();
    if (status != 0) std::fprintf(stderr, "[lgnc] close failed: %d\n", status);
}

FeedResult LgncVideo::feed(const void *data, size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    const bool annex_b = size >= 4 && bytes[0] == 0 && bytes[1] == 0 &&
        (bytes[2] == 1 || (bytes[2] == 0 && bytes[3] == 1));
    if (!annex_b) return FeedResult::Error;
    const size_t header = bytes[2] == 1 ? 3 : 4;
    if (size <= header) return FeedResult::Error;
    if ((bytes[header] & 31) != 9) {
        // The demuxer supplies one access unit per packet, but MP4 may omit AUD.
        access_unit_.assign({0, 0, 0, 1, 9, 0xf0});
        access_unit_.insert(access_unit_.end(), bytes, bytes + size);
        data = access_unit_.data();
        size = access_unit_.size();
    }
    const int status = play_(data, static_cast<unsigned>(size));
    if (status == 0) return FeedResult::Accepted;
    if (status == LGNC_BUFFER_FULL) return FeedResult::Retry;
    std::fprintf(stderr, "[lgnc] video feed failed: %d\n", status);
    return FeedResult::Error;
}
