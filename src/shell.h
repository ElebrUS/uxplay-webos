#pragma once
#include <SDL.h>
#include <array>
#include <cstdint>
#include <string>
enum class Action { None, Back, Exit, Left, Right, LeftRepeat, RightRepeat, SeekRelease, Select };
class Shell {
public:
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    bool open();
    bool load_assets(const std::string &directory);
    bool pump();
    Action action();
    void screen(const std::string& status, const std::string& pin, int audio_delay_ms=100);
    void transparent();
    void playback_loader();
    void playback_controls(int position, int duration, bool paused, int target = -1, bool confirm_seek = false);
    ~Shell();
private:
    enum class Align { Left, Center, Right };
    void text(const std::string &label, int x, int y, int cap_height, int max_width,
              SDL_Color color, Align align);
    SDL_Texture *glyphs_ = nullptr;
    SDL_Texture *backdrop_ = nullptr;
    std::array<uint8_t, 95> advances_{};
};
int64_t now_ns();
std::string app_directory();
void start_log(const char *name);
void rotate_log();
