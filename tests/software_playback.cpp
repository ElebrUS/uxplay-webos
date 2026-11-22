#include "media.h"
#include <SDL.h>
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

static bool verify_pixels(SDL_Renderer *renderer, bool portrait) {
    Uint32 pixels[320 * 240];
    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                             pixels, 320 * sizeof(Uint32)) != 0) return false;
    const auto rgb = [&](int x, int y) { return pixels[y * 320 + x] & 0xffffff; };
    return rgb(160, 120) != 0 && (portrait ? rgb(0, 120) : rgb(160, 0)) == 0;
}

static bool play(Media &media, SDL_Renderer *renderer, const char *path, bool portrait, bool buffered = false) {
    std::ifstream input(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    const int width = portrait ? 360 : 640, height = portrait ? 640 : 360;
    if (bytes.empty() || !(buffered ? media.open_clip(width, height, true) :
                                    media.open(nullptr, width, height, true))) return false;
    if (buffered) media.show_frames_from(30 * 33333333LL);
    size_t remaining = bytes.size();
    bytes.resize(remaining + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    const uint8_t *cursor = bytes.data();
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_H264);
    AVCodecContext *context = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    const auto start = std::chrono::steady_clock::now();
    bool ok = parser && context;
    int packets = 0;
    while (ok) {
        uint8_t *unit = nullptr;
        int size = 0;
        const int used = av_parser_parse2(parser, context, &unit, &size, cursor, int(remaining),
                                          AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0 || (used == 0 && size == 0 && remaining)) { ok = false; break; }
        if (size) {
            if (media.feed(unit, size, packets++ * 33333333LL, true) != FeedResult::Accepted) {
                ok = false;
                break;
            }
            if (buffered && packets <= 30 && media.frames != 0) ok = false;
        }
        if (!remaining && !size) break;
        cursor += used;
        remaining -= used;
    }
    if (buffered) media.eos();
    const unsigned frames = media.frames.load();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    uint8_t pcm[2048] = {};
    ok = ok && !media.failed && frames == (buffered ? 30u : 60u) && verify_pixels(renderer, portrait) &&
         media.feed(pcm, sizeof(pcm), 0, false) == FeedResult::Accepted;
    media.pause(true);
    media.pause(false);
    unsigned overlay_draws = 0;
    media.set_overlay([&] { ++overlay_draws; });
    media.begin_loading(true);
    media.close();
    media.redraw();
    ok = ok && overlay_draws == 2 && verify_pixels(renderer, portrait);
    media.end_loading();
    media.set_overlay({});
    avcodec_free_context(&context);
    if (parser) av_parser_close(parser);
    std::fprintf(stderr, "%s: %u frames in %lld ms, pixel and lifecycle checks %s\n",
                 path, frames, static_cast<long long>(ms), ok ? "passed" : "failed");
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 3 || SDL_Init(0) != 0) return 2;
    SDL_Surface *surface = SDL_CreateRGBSurface(0, 320, 240, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
    SDL_Renderer *renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    bool ok = false;
    if (renderer) {
        Media media(renderer);
        ok = play(media, renderer, argv[1], false) && play(media, renderer, argv[2], true) &&
             play(media, renderer, argv[1], false, true);
        SDL_DestroyRenderer(renderer);
    }
    if (surface) SDL_FreeSurface(surface);
    SDL_Quit();
    return ok ? 0 : 1;
}
