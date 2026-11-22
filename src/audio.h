#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
struct AVCodecContext;
struct SwrContext;
class AudioDecoder {
public:
    ~AudioDecoder();
    bool decode(unsigned codec, const uint8_t *data, size_t size, std::vector<uint8_t>& pcm);
    void reset();
    bool configure_aac(const uint8_t *data, size_t length);
private:
    bool configure(unsigned codec, const uint8_t *data = nullptr, size_t length = 0, bool use_default_config = true);
    unsigned codec_ = 0;
    AVCodecContext *context_ = nullptr;
    SwrContext *resampler_ = nullptr;
};
