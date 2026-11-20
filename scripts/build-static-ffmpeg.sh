#!/usr/bin/env bash
set -euo pipefail

export PATH="/ucrt64/bin:/usr/bin:$PATH"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FF_SRC="$ROOT_DIR/third_party/ffmpeg-8.0"
FF_BUILD="$ROOT_DIR/build/ffmpeg-static-build"
FF_INSTALL="$ROOT_DIR/third_party/ffmpeg-static-install"

mkdir -p "$FF_BUILD"
cd "$FF_BUILD"

echo "=== Configuring minimal static FFmpeg 8.0 (H.264/HEVC + D3D11VA + AAC/ALAC) ==="
"$FF_SRC/configure" \
    --prefix="$FF_INSTALL" \
    --arch=x86_64 \
    --target-os=mingw32 \
    --cc=gcc \
    --cxx=g++ \
    --enable-static \
    --disable-shared \
    --disable-programs \
    --disable-doc \
    --disable-debug \
    --disable-network \
    --disable-autodetect \
    --disable-avformat \
    --disable-avdevice \
    --disable-avfilter \
    --disable-everything \
    --enable-d3d11va \
    --enable-decoder=h264,hevc,aac,alac \
    --enable-hwaccel=h264_d3d11va,h264_d3d11va2,hevc_d3d11va,hevc_d3d11va2 \
    --enable-swresample \
    --enable-swscale

echo "=== Building minimal static FFmpeg 8.0 ==="
make -j"$(nproc)"
make install

echo "=== Static FFmpeg 8.0 installed to $FF_INSTALL ==="
ls -lh "$FF_INSTALL/lib"
