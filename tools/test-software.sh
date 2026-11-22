#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir="${TEST_BUILD_DIR:-build/software-test}"
mkdir -p "$build_dir"
"${CXX:-c++}" -std=c++11 -Wall -Wextra -Isrc tests/hls_playlist.cpp src/hls_playlist.cpp \
    -o "$build_dir/hls-playlist"
"$build_dir/hls-playlist"
for dimensions in 640x360 360x640; do
    "${FFMPEG:-ffmpeg}" -hide_banner -loglevel error -y -f lavfi \
        -i "testsrc2=size=${dimensions}:rate=30" -frames:v 60 \
        -c:v libx264 -preset ultrafast -tune zerolatency -f h264 "$build_dir/$dimensions.h264"
done
# pkg-config emits compiler and linker arguments as separate shell words.
# shellcheck disable=SC2046
"${CXX:-c++}" -std=c++11 -O2 -DWEBOS14 -Isrc $(pkg-config --cflags sdl2 libavcodec libavutil) \
    tests/software_playback.cpp src/media_sdl.cpp -o "$build_dir/software-playback" \
    $(pkg-config --libs sdl2 libavcodec libavutil)
SDL_AUDIODRIVER=dummy "$build_dir/software-playback" "$build_dir/640x360.h264" "$build_dir/360x640.h264"
