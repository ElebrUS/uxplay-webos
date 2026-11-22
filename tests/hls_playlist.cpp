#include "hls_playlist.h"
#include <cassert>
#include <string>

int main() {
    const std::string master =
        "#EXTM3U\r\n#EXT-X-VERSION:7\r\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\"Alt\",DEFAULT=NO,URI=\"alt.m3u8\"\r\n"
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\"Main\",DEFAULT=YES,URI=\"audio/main.m3u8\"\r\n"
        "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"sub\",URI=\"sub.m3u8\"\r\n"
        "#EXT-X-STREAM-INF:RESOLUTION=3840x2160,CODECS=\"hvc1.1.6.L150.B0\"\r\nhevc.m3u8\r\n"
        "#EXT-X-STREAM-INF:RESOLUTION=1280x720,CODECS=\"avc1.640028,mp4a.40.2\",AUDIO=\"audio\"\r\n720.m3u8\r\n"
        "#EXT-X-STREAM-INF:RESOLUTION=640x360,CODECS=\"avc1.64001f,mp4a.40.2\",AUDIO=\"audio\",SUBTITLES=\"sub\"\r\n"
        "video/360.m3u8\r\n";
    const auto filtered = select_hls_rendition(master);
    assert(filtered == "#EXTM3U\n#EXT-X-VERSION:7\n"
                       "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\"Main\",DEFAULT=YES,URI=\"audio/main.m3u8\"\n"
                       "#EXT-X-STREAM-INF:RESOLUTION=640x360,CODECS=\"avc1.64001f,mp4a.40.2\",AUDIO=\"audio\"\n"
                       "video/360.m3u8\n");
    const std::string media = "#EXTM3U\n#EXTINF:6,\nclip.ts\n";
    assert(select_hls_rendition(media) == media);
    const std::string mixed_rates = master +
        "#EXT-X-STREAM-INF:RESOLUTION=854x480,FRAME-RATE=59.94,BANDWIDTH=2000000\n480-fast.m3u8\n"
        "#EXT-X-STREAM-INF:RESOLUTION=854x480,FRAME-RATE=23.976,BANDWIDTH=1500000\n480-heavy.m3u8\n"
        "#EXT-X-STREAM-INF:RESOLUTION=854x480,FRAME-RATE=23.976,BANDWIDTH=900000\n480-light.m3u8\n";
    assert(select_hls_rendition(mixed_rates).find("\n480-light.m3u8\n") != std::string::npos);
    assert(select_hls_rendition(master +
        "#EXT-X-STREAM-INF:RESOLUTION=854x480,FRAME-RATE=59.94\nfast.m3u8\n")
        .find("\nvideo/360.m3u8\n") != std::string::npos);
}
