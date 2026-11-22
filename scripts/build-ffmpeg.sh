#!/usr/bin/env bash
# Cross-compiles a minimal, decode-only FFmpeg for the PS3 PPU (PSL1GHT toolchain).
# Usage: PS3DEV=... PSL1GHT=... scripts/build-ffmpeg.sh <install-prefix>
set -euo pipefail

: "${PS3DEV:?PS3DEV must be set}"
: "${PSL1GHT:?PSL1GHT must be set}"

FFVER="${FFVER:-4.4.5}"
PREFIX="${1:-$PS3DEV/portlibs/ppu}"
WORK="${WORK:-$(pwd)/_ffmpeg_build}"
PORTLIBS="$PS3DEV/portlibs/ppu"
export PATH="$PS3DEV/bin:$PS3DEV/ppu/bin:$PATH"

mkdir -p "$WORK"
cd "$WORK"
if [ ! -f "ffmpeg-$FFVER.tar.xz" ]; then
    curl -fsSL -o "ffmpeg-$FFVER.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$FFVER.tar.xz"
fi

echo '#undef __STRICT_ANSI__' > "$WORK/nostrictansi.h"

CFL="-mcpu=cell -mminimal-toc -O2 -DPS3 -I$PSL1GHT/ppu/include -I$PORTLIBS/include -include $WORK/nostrictansi.h"
LDF="-L$PSL1GHT/ppu/lib -L$PORTLIBS/lib"

COMMON=(
    --prefix="$PREFIX"
    --enable-cross-compile --cross-prefix=ppu- --arch=ppc64 --cpu=cell --target-os=none
    --enable-static --disable-shared
    --disable-programs --disable-doc --disable-debug
    --disable-autodetect --disable-runtime-cpudetect
    --disable-network --disable-protocols --disable-devices --disable-avdevice
    --disable-avfilter --disable-postproc
    --disable-pthreads --disable-w32threads --disable-os2threads
    --disable-zlib --disable-bzlib --disable-lzma --disable-iconv
    --disable-everything
    --enable-avformat --enable-avcodec --enable-swresample --enable-swscale
    --enable-demuxer=mpegts,mpegps,flv,mov,matroska,aac,mp3,ac3,h264,mpegvideo,m4v
    --enable-decoder=h264,mpeg2video,mpeg1video,mpeg4,aac,aac_latm,mp2,mp2float,mp3,mp3float,ac3,eac3
    --enable-parser=h264,aac,aac_latm,mpegaudio,mpegvideo,mpeg4video,ac3
    --extra-cflags="$CFL"
    --extra-ldflags="$LDF"
    --extra-libs="-lrt -llv2 -lm"
)

try_build() {
    local label="$1"; shift
    echo "=== FFmpeg $FFVER build attempt: $label ==="
    rm -rf "ffmpeg-$FFVER"
    tar xf "ffmpeg-$FFVER.tar.xz"
    (
        cd "ffmpeg-$FFVER"
        if ! ./configure "${COMMON[@]}" "$@"; then
            echo "--- configure failed, tail of config.log ---"
            tail -n 60 ffbuild/config.log || true
            exit 1
        fi
        make -j"$(nproc)" && make install
    )
}

if try_build "altivec"; then
    :
elif try_build "no-altivec" --disable-altivec; then
    :
elif try_build "no-asm" --disable-asm; then
    :
else
    echo "FFmpeg build failed in every configuration" >&2
    exit 1
fi

echo "FFmpeg installed to $PREFIX"
ls -la "$PREFIX/lib"
