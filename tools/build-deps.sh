#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
sdk="$root/.cache/arm-webos-linux-gnueabi_sdk-buildroot"
prefix="$root/.cache/deps"
export PATH="$sdk/bin:$PATH"
export CC="$sdk/bin/arm-webos-linux-gnueabi-gcc" CXX="$sdk/bin/arm-webos-linux-gnueabi-g++"
export AR="$sdk/bin/arm-webos-linux-gnueabi-ar" RANLIB="$sdk/bin/arm-webos-linux-gnueabi-ranlib"
export CFLAGS="--sysroot=$sdk/arm-webos-linux-gnueabi/sysroot -O2 -fPIC"
export CXXFLAGS="$CFLAGS -D_GLIBCXX_USE_CXX11_ABI=0"
export LDFLAGS="--sysroot=$sdk/arm-webos-linux-gnueabi/sysroot"
mkdir -p "$prefix" build/logs
build_stamp="$(shasum -a 256 "$0" | cut -d ' ' -f 1)"
previous_stamp="$(cat "$prefix/build-config.sha256" 2>/dev/null || true)"
if [ ! -f "$prefix/lib/libcrypto.a" ] || [ ! -f "$prefix/lib/libjitterentropy.a" ] ||
   ! grep -q libjitterentropy "$prefix/lib/pkgconfig/libcrypto.pc"; then
    jitter="$root/.cache/jitterentropy-library-3.6.3"
    make -C "$jitter" CC="$CC" AR="$AR" jitterentropy-static >build/logs/jitterentropy.log 2>&1
    cp "$jitter/libjitterentropy.a" "$prefix/lib/"

    (cd .cache/openssl-3.5.8
     if [ -f Makefile ]; then make clean; fi
     ./Configure linux-armv4 no-shared no-tests no-module no-async enable-jitter \
         --with-jitter-include="$jitter" --with-jitter-lib="$prefix/lib" \
         -DOPENSSL_DEFAULT_SEED_SRC=JITTER --prefix="$prefix" --libdir=lib
     make -j"${BUILD_JOBS:-2}" build_libs
     make install_dev) >build/logs/openssl.log 2>&1
fi
if [ ! -f "$prefix/lib/libplist-2.0.a" ]; then
    (cd .cache/libplist-2.7.0
     ./configure --host=arm-webos-linux-gnueabi --prefix="$prefix" --disable-shared --enable-static --without-cython --without-tests
     make -j"${BUILD_JOBS:-2}"
     make install) >build/logs/libplist.log 2>&1
fi
if [ "$build_stamp" != "$previous_stamp" ] || [ ! -f "$prefix/lib/libavformat.a" ]; then
    (cd .cache/ffmpeg-8.1.2
     export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
     export PKG_CONFIG_PATH=""
     make clean
     ./configure --prefix="$prefix" --arch=arm --cpu=cortex-a9 --target-os=linux --enable-cross-compile \
       --sysroot="$sdk/arm-webos-linux-gnueabi/sysroot" \
       --extra-cflags=-Dstatic_assert=_Static_assert \
       --cross-prefix="$sdk/bin/arm-webos-linux-gnueabi-" --disable-everything --disable-programs --disable-doc \
       --disable-shared --enable-static --enable-pic --disable-autodetect --enable-network --enable-openssl --enable-version3 --pkg-config-flags=--static \
       --disable-avdevice --enable-avformat --disable-avfilter --disable-swscale \
       --enable-avcodec --enable-avutil --enable-swresample --enable-decoder=aac,alac,pcm_s16le,h264 --disable-zlib \
       --enable-demuxer=mov,hls,mpegts,aac --enable-protocol=http,https,tcp,tls,udp --enable-bsf=h264_mp4toannexb --enable-parser=h264,aac --enable-small --disable-debug
     make -j"${BUILD_JOBS:-2}"
     make install) >build/logs/ffmpeg.log 2>&1
fi
printf '%s\n' "$build_stamp" > "$prefix/build-config.sha256"
echo "Dependencies ready in $prefix"
