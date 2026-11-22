#include "shell.h"
#include <SDL_webOS.h>
#include "font8x8_basic.h"
#include <algorithm>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
static std::string log_path;
static volatile sig_atomic_t interrupted = 0;
static void signal_stop(int) { interrupted = 1; }
int64_t now_ns() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return int64_t(t.tv_sec)*1000000000 + t.tv_nsec; }
std::string app_directory() {
    char p[4096]; ssize_t n = readlink("/proc/self/exe", p, sizeof(p)-1);
    if (n <= 0) return ".";
    p[n] = 0; std::string s(p); return s.substr(0, s.rfind('/'));
}
void start_log(const char *name) {
    std::string p = app_directory() + "/" + name;
    log_path=p;
    rename(p.c_str(), (p+".previous").c_str());
    freopen(p.c_str(), "w", stderr);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    // Proprietary drivers emit high-volume frame diagnostics to stdout.
    // Keep application/errors on stderr, without filling app storage with them.
    int nullfd=open("/dev/null",O_WRONLY);
    if(nullfd>=0){dup2(nullfd,STDOUT_FILENO);close(nullfd);}
}
void rotate_log() {
    struct stat s;
    if(!log_path.empty() && fstat(fileno(stderr),&s)==0 && s.st_size>1024*1024) {
        rename(log_path.c_str(),(log_path+".previous").c_str());
        freopen(log_path.c_str(),"w",stderr);setvbuf(stderr,nullptr,_IOLBF,0);
    }
}
bool Shell::open() {
    signal(SIGTERM, signal_stop); signal(SIGINT, signal_stop);
    setenv("EGL_PLATFORM", "wayland", 0); setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0);
#ifdef WEBOS14
    setenv("WAYLAND_DISPLAY", "wayland-0", 0);
    setenv("SDL_VIDEODRIVER", "wayland", 0);
#endif
    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK, "true");
    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_EXIT, "true");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "[SDL] %s\n",SDL_GetError()); return false; }
    SDL_DisplayMode mode{};
    if (SDL_GetCurrentDisplayMode(0, &mode) != 0 || mode.w <= 0 || mode.h <= 0) {
#ifdef WEBOS14
        mode.w = 1920;
        mode.h = 1080;
#else
        return false;
#endif
    }
    window = SDL_CreateWindow("UxPlay webOS",0,0,mode.w,mode.h,SDL_WINDOW_FULLSCREEN);
    if (window) renderer = SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED);
    #ifdef WEBOS14
    if (window && !renderer) renderer = SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
#endif
    if (!renderer) { fprintf(stderr,"[SDL] %s\n",SDL_GetError()); return false; }
    int width = 0, height = 0;
    SDL_GetRendererOutputSize(renderer, &width, &height);
    fprintf(stderr, "[SDL] window %dx%d, renderer %dx%d\n", mode.w, mode.h, width, height);
    const bool atlas_loaded = load_assets(app_directory());
    fprintf(stderr, "[ui] %s\n", atlas_loaded ? "Inter atlas loaded" : "Inter unavailable; bitmap fallback");

    return true;
}
void Shell::transparent() {
    SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer,0,0,0,0); SDL_RenderClear(renderer); SDL_RenderPresent(renderer);
}
Action Shell::action() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return Action::Exit;
        if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) return Action::Select;
        if (e.type == SDL_KEYUP &&
            (e.key.keysym.scancode == SDL_SCANCODE_LEFT || e.key.keysym.scancode == SDL_SCANCODE_RIGHT))
            return Action::SeekRelease;
        if (e.type == SDL_KEYDOWN) {
            const auto key = e.key.keysym.scancode;
            if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER ||
                key == SDL_SCANCODE_SELECT || key == SDL_SCANCODE_SPACE || key == SDL_SCANCODE_AUDIOPLAY) {
                if (!e.key.repeat) return Action::Select;
                continue;
            }
            if (key == SDL_SCANCODE_LEFT) return e.key.repeat ? Action::LeftRepeat : Action::Left;
            if (key == SDL_SCANCODE_RIGHT) return e.key.repeat ? Action::RightRepeat : Action::Right;
            if(e.key.keysym.scancode==SDL_WEBOS_SCANCODE_EXIT) return Action::Exit;
            if(e.key.keysym.scancode==SDL_WEBOS_SCANCODE_BACK || e.key.keysym.scancode==SDL_SCANCODE_ESCAPE || e.key.keysym.scancode==SDL_SCANCODE_AC_BACK) return Action::Back;
        }
    }
    return interrupted?Action::Exit:Action::None;
}
bool Shell::pump() {Action a=action();return a!=Action::Back&&a!=Action::Exit;}

namespace {
constexpr SDL_Color primary = {247, 249, 255, 255};
constexpr SDL_Color secondary = {170, 183, 204, 255};
constexpr SDL_Color accent = {145, 190, 255, 255};
constexpr int cell = 76, columns = 16, rows = 6;

struct Canvas {
    explicit Canvas(SDL_Renderer *renderer) {
        if (SDL_GetRendererOutputSize(renderer, &width, &height) != 0) width = height = 0;
        scale = std::min(width / 1920.0, height / 1080.0);
        left = (width - units(1920)) / 2;
        top = (height - units(1080)) / 2;
    }
    int units(int v) const { return int(std::lround(v * scale)); }
    int x(int v) const { return left + units(v); }
    int y(int v) const { return top + units(v); }
    SDL_Rect box(int x0, int y0, int w, int h) const { return {x(x0), y(y0), units(w), units(h)}; }
    int width = 0, height = 0, left = 0, top = 0;
    double scale = 0;
};

void color(SDL_Renderer *renderer, SDL_Color value) {
    SDL_SetRenderDrawColor(renderer, value.r, value.g, value.b, value.a);
}

void rounded(SDL_Renderer *renderer, SDL_Rect rect, int radius, SDL_Color fill) {
    if (rect.w <= 0 || rect.h <= 0) return;
    color(renderer, fill);
    radius = std::max(0, std::min(radius, std::min(rect.w, rect.h) / 2));
    if (!radius) { SDL_RenderFillRect(renderer, &rect); return; }
    const SDL_Rect body = {rect.x, rect.y + radius, rect.w, rect.h - 2 * radius};
    if (body.h) SDL_RenderFillRect(renderer, &body);
    for (int row = 0; row < radius; ++row) {
        const double dy = radius - row - 0.5;
        const int inset = radius - int(std::sqrt(radius * radius - dy * dy));
        if (2 * inset >= rect.w) continue;
        SDL_RenderDrawLine(renderer, rect.x + inset, rect.y + row, rect.x + rect.w - 1 - inset, rect.y + row);
        const int bottom = rect.y + rect.h - 1 - row;
        SDL_RenderDrawLine(renderer, rect.x + inset, bottom, rect.x + rect.w - 1 - inset, bottom);
    }
}

void panel(SDL_Renderer *renderer, const Canvas &c, int x, int y, int w, int h,
           int radius, SDL_Color fill) {
    rounded(renderer, c.box(x, y, w, h), c.units(radius), {125, 155, 211, 100});
    rounded(renderer, c.box(x + 1, y + 1, w - 2, h - 2), c.units(radius - 1), fill);
}

void display_icon(SDL_Renderer *renderer, const Canvas &c, int x, int y, bool mirroring) {
    rounded(renderer, c.box(x, y, 52, 37), c.units(7), primary);
    rounded(renderer, c.box(x + 3, y + 3, 46, 31), c.units(4), {37, 53, 82, 255});
    if (mirroring) {
        rounded(renderer, c.box(x + 30, y + 18, 19, 32), c.units(5), accent);
        rounded(renderer, c.box(x + 33, y + 21, 13, 23), c.units(2), {37, 53, 82, 255});
    } else {
        color(renderer, accent);
        const int length = c.units(17);
        for (int i = 0; i < length; ++i) {
            const int half = (length - i) / 2;
            SDL_RenderDrawLine(renderer, c.x(x + 20) + i, c.y(y + 19) - half,
                              c.x(x + 20) + i, c.y(y + 19) + half);
        }
    }
}
}

bool Shell::load_assets(const std::string &directory) {
    if (glyphs_) return true;
    std::ifstream file(directory + "/ui-glyphs.bin", std::ios::binary);
    constexpr int width = columns * cell, height = rows * cell;
    std::vector<uint8_t> alpha(width * height);
    if (!file.read(reinterpret_cast<char *>(advances_.data()), advances_.size()) ||
        !file.read(reinterpret_cast<char *>(alpha.data()), alpha.size())) return false;
    SDL_Surface *surface = SDL_CreateRGBSurface(0, width, height, 32,
                                               0xff0000, 0x00ff00, 0x0000ff, 0xff000000);
    if (!surface) return false;
    if (SDL_LockSurface(surface) != 0) { SDL_FreeSurface(surface); return false; }
    for (int y = 0; y < height; ++y) {
        auto *row = reinterpret_cast<Uint32 *>(static_cast<uint8_t *>(surface->pixels) + y * surface->pitch);
        for (int x = 0; x < width; ++x) row[x] = 0x00ffffff | (Uint32(alpha[y * width + x]) << 24);
    }
    SDL_UnlockSurface(surface);
    glyphs_ = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (glyphs_) SDL_SetTextureBlendMode(glyphs_, SDL_BLENDMODE_BLEND);
    return glyphs_ != nullptr;
}

void Shell::text(const std::string &label, int x, int y, int cap_height, int max_width,
                 SDL_Color tint, Align align) {
    if (label.empty() || cap_height < 1 || max_width < 1) return;
    const auto index = [](unsigned char c) { return int(c >= 32 && c <= 126 ? c : '?') - 32; };
    int advance = 0;
    for (unsigned char c : label) advance += glyphs_ ? advances_[index(c)] : 8;
    if (advance == 0) return;
    const double scale = std::min(cap_height / (glyphs_ ? 42.0 : 8.0), double(max_width) / advance);
    const int width = int(std::lround(advance * scale));
    if (align == Align::Center) x -= width / 2;
    if (align == Align::Right) x -= width;
    if (glyphs_) {
        SDL_SetTextureColorMod(glyphs_, tint.r, tint.g, tint.b);
        SDL_SetTextureAlphaMod(glyphs_, tint.a);
        const int size = std::max(1, int(std::lround(cell * scale)));
        int cursor = 0;
        for (unsigned char c : label) {
            const int g = index(c);
            if (c != ' ') {
                const SDL_Rect src = {(g % columns) * cell, (g / columns) * cell, cell, cell};
                const SDL_Rect dest = {x + int(std::lround((cursor - 3) * scale)),
                                       y - int(std::lround(37 * scale)), size, size};
                SDL_RenderCopy(renderer, glyphs_, &src, &dest);
            }
            cursor += advances_[g];
        }
    } else {
        color(renderer, tint);
        const int pixel = std::max(1, int(scale));
        for (unsigned char c : label) {
            for (int row = 0; row < 8; ++row) {
                const unsigned bits = font8x8_basic[index(c) + 32][row];
                for (int column = 0; column < 8; ++column) {
                    if (bits & (1u << column)) {
                        const SDL_Rect dot = {x + column * pixel, y + (row - 4) * pixel, pixel, pixel};
                        SDL_RenderFillRect(renderer, &dot);
                    }
                }
            }
            x += 8 * pixel;
        }
    }
}

void Shell::screen(const std::string &status, const std::string &pin, int delay) {
    const Canvas c(renderer);
    if (c.scale <= 0) return;
    if (!backdrop_) {
        constexpr int w = 256, h = 144;
        std::vector<Uint32> pixels(w * h);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const double dx = x / double(w), dy = y / double(h);
                const double blue = std::exp(-((dx - 0.26) * (dx - 0.26) * 10 + dy * dy * 3.8));
                const double lilac = std::exp(-((dx - 0.91) * (dx - 0.91) * 18 + (dy - 0.2) * (dy - 0.2) * 9));
                const auto rgb = [&](int base, int b, int l) { return Uint32(base + b * blue + l * lilac); };
                pixels[y * w + x] = 0xff000000 | rgb(8, 15, 23) << 16 | rgb(12, 32, 14) << 8 | rgb(23, 61, 38);
            }
        }
        backdrop_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (backdrop_ && SDL_UpdateTexture(backdrop_, nullptr, pixels.data(), w * sizeof(Uint32)) != 0) {
            SDL_DestroyTexture(backdrop_);
            backdrop_ = nullptr;
        }
    }
    color(renderer, {9, 15, 28, 255});
    SDL_RenderClear(renderer);
    if (backdrop_) SDL_RenderCopy(renderer, backdrop_, nullptr, nullptr);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const auto label = [&](const std::string &s, int x, int y, int size, int max, SDL_Color tint, Align align) {
        text(s, c.x(x), c.y(y), c.units(size), c.units(max), tint, align);
    };
    const bool idle = pin.empty() && status == "Waiting for a connection";
    const bool pairing = !pin.empty();
    if (idle) {
        rounded(renderer, c.box(204, 300, 84, 84), c.units(24), {56, 84, 132, 255});
        display_icon(renderer, c, 222, 320, true);
        label("AirPlay", 204, 463, 86, 620, primary, Align::Left);
        label("Share from iPhone, iPad or Mac.", 204, 565, 27, 630, secondary, Align::Left);
        label("Connect both devices to the same network.", 204, 615, 20, 630, secondary, Align::Left);

        panel(renderer, c, 864, 273, 852, 485, 42, {23, 34, 56, 237});
        label("Choose this receiver", 925, 337, 21, 695, secondary, Align::Left);
        const char *name = getenv("UXPLAY_NAME");
        label(name && *name ? name : "UxPlay webOS", 925, 409, 45, 695, primary, Align::Left);
        color(renderer, {113, 137, 182, 110});
        SDL_RenderDrawLine(renderer, c.x(925), c.y(470), c.x(1655), c.y(470));

        display_icon(renderer, c, 925, 505, true);
        label("Screen Mirroring", 1004, 513, 27, 590, primary, Align::Left);
        label("Open Control Center and choose Screen Mirroring.", 1004, 556, 19, 620, secondary, Align::Left);
        display_icon(renderer, c, 925, 631, false);
        label("Video from Safari", 1004, 637, 27, 590, primary, Align::Left);
        label("Use AirPlay in the video player.", 1004, 680, 19, 610, secondary, Align::Left);
    } else {
        rounded(renderer, c.box(918, 172, 84, 84), c.units(24), {56, 84, 132, 255});
        display_icon(renderer, c, 934, 191, true);
        label(pairing ? "Pair your device" : "UxPlay", 960, 343, 78, 1320, primary, Align::Center);
        if (pairing) {
            label("Enter this code on the device you are connecting.", 960, 432, 25, 1340, secondary, Align::Center);
            const int start = 960 - int(pin.size()) * 66;
            for (size_t i = 0; i < pin.size(); ++i) {
                const int x = start + int(i) * 132;
                panel(renderer, c, x + 8, 535, 116, 148, 26, {30, 44, 72, 250});
                label(std::string(1, pin[i]), x + 66, 611, 60, 90, primary, Align::Center);
            }
        } else {
            panel(renderer, c, 420, 496, 1080, 142, 32, {30, 43, 70, 242});
            label(status, 960, 568, 30, 1000, primary, Align::Center);
        }
    }

    if (!pairing) {
        char sync[80];
        snprintf(sync, sizeof(sync), "Audio delay  %+d ms  |  Left and right to adjust", delay);
        label(sync, 204, 926, 18, 900, secondary, Align::Left);
    }
    label("Back to exit", 1716, 926, 18, 340, secondary, Align::Right);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_RenderPresent(renderer);
}

Shell::~Shell() {
    if (backdrop_) SDL_DestroyTexture(backdrop_);
    if (glyphs_) SDL_DestroyTexture(glyphs_);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
}

void Shell::playback_loader() {
    const Canvas c(renderer);
    if (c.scale <= 0) return;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    color(renderer, {10, 16, 27, 100});
    SDL_RenderFillRect(renderer, nullptr);
    rounded(renderer, c.box(900, 480, 120, 120), c.units(36), {15, 22, 39, 207});
    const int step = int(now_ns() / 80000000 % 12);
    for (int i = 0; i < 12; ++i) {
        const double angle = i * 6.283185307 / 12;
        rounded(renderer, c.box(955 + int(30 * std::cos(angle)), 535 + int(30 * std::sin(angle)), 10, 10),
                c.units(5), {213, 231, 255, Uint8(255 - ((step - i + 12) % 12) * 18)});
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

void Shell::playback_controls(int position, int duration, bool paused, int target, bool confirm_seek) {
    const Canvas c(renderer);
    if (c.scale <= 0) return;
    position = std::max(0, std::min(position, duration));
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    panel(renderer, c, 410, 799, 1100, 224, 42, {15, 21, 34, 233});

    for (int x : {810, 960, 1110}) {
        if (target >= 0 && x == 960) continue;
        const bool center = x == 960;
        rounded(renderer, c.box(x - 39, 825, 78, 78), c.units(39),
                center ? SDL_Color{218, 233, 255, 255} : SDL_Color{126, 152, 195, 51});
    }
    color(renderer, {20, 34, 59, 255});
    if (target < 0 && paused) {
        for (int i = 0; i < c.units(23); ++i) {
            const int half = c.units(15) * (c.units(23) - i) / std::max(1, c.units(23));
            SDL_RenderDrawLine(renderer, c.x(950) + i, c.y(864) - half,
                              c.x(950) + i, c.y(864) + half);
        }
    } else if (target < 0) {
        for (int x : {949, 963}) {
            const SDL_Rect bar = c.box(x, 850, 8, 28);
            SDL_RenderFillRect(renderer, &bar);
        }
    }
    text("-10", c.x(810), c.y(864), c.units(22), c.units(60), primary, Align::Center);
    text("+10", c.x(1110), c.y(864), c.units(22), c.units(60), primary, Align::Center);
    rounded(renderer, c.box(467, 928, 986, 6), c.units(3), {154, 175, 209, 91});
    const int progress = duration > 0 ? int(int64_t(986) * position / duration) : 0;
    const int cursor = target >= 0 && duration > 0
        ? int(int64_t(986) * std::min(target, duration) / duration) : progress;
    rounded(renderer, c.box(467, 928, progress, 6), c.units(3), accent);
    rounded(renderer, c.box(459 + cursor, 923, 16, 16), c.units(8), target >= 0 ? primary : accent);

    char elapsed[24], total[24];
    snprintf(elapsed, sizeof(elapsed), "%02d:%02d", position / 60, position % 60);
    snprintf(total, sizeof(total), "%02d:%02d", duration / 60, duration % 60);
    text(elapsed, c.x(467), c.y(968), c.units(19), c.units(170), secondary, Align::Left);
    text(total, c.x(1453), c.y(968), c.units(19), c.units(170), secondary, Align::Right);
    if (target >= 0) {
        char preview[32];
        snprintf(preview, sizeof(preview), "%02d:%02d", target / 60, target % 60);
        text(preview, c.x(960), c.y(864), c.units(27), c.units(220), primary, Align::Center);
        text(confirm_seek ? "OK to seek  |  Back to cancel" : "Back to cancel", c.x(960), c.y(968), c.units(18),
             c.units(470), primary, Align::Center);
    } else {
        text(paused ? "OK to resume" : "OK to pause", c.x(960), c.y(968), c.units(18),
             c.units(440), primary, Align::Center);
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}
