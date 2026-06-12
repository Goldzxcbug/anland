#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NDK_DIR="$SCRIPT_DIR/build_tools/android-sdk/ndk/29.0.13113456"
BUILD_DIR="$SCRIPT_DIR/build_consumer"
TOOLCHAIN="$NDK_DIR/build/cmake/android.toolchain.cmake"

if [ ! -f "$TOOLCHAIN" ]; then
    echo "NDK toolchain not found at $TOOLCHAIN"
    exit 1
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$SCRIPT_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-30 \
    -DCMAKE_BUILD_TYPE=Release

cmake --build . --target display_consumer -- -j$(nproc)

echo "Built: $BUILD_DIR/libdisplay_consumer.so"
