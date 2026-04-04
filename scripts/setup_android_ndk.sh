#!/bin/bash
# setup_android_ndk.sh
# Downloads ffmpeg-kit and configures the build environment for Android.
#
# Usage:
#   ./scripts/setup_android_ndk.sh [abi]
#
# Supported ABIs: arm64-v8a (default), armeabi-v7a, x86_64, x86

set -e

ABI="${1:-arm64-v8a}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ANDROID_SDK_DIR="${ANDROID_SDK_DIR:-$HOME/android-sdk}"
NDK_DIR="${ANDROID_SDK_DIR}/android-ndk-r27c"
FFMPEG_KIT_DIR="${ANDROID_SDK_DIR}/ffmpeg-kit"

echo "╔══════════════════════════════════════════════════════╗"
echo "║  batchpress — Android NDK Setup                    ║"
echo "╚══════════════════════════════════════════════════════╝"
echo ""
echo "  ABI:         $ABI"
echo "  NDK:         $NDK_DIR"
echo "  FFMPEG-KIT:  $FFMPEG_KIT_DIR"
echo ""

# ── Check NDK ─────────────────────────────────────────────────────────────────
if [ ! -f "$NDK_DIR/build/cmake/android.toolchain.cmake" ]; then
    echo "[ERROR] Android NDK not found at $NDK_DIR"
    echo ""
    echo "  Download from: https://developer.android.com/ndk/downloads"
    echo "  Or run: sdkmanager \"ndk;27.2.12479018\""
    exit 1
fi
echo "[OK] NDK found: $NDK_DIR"

# ── Check/Download ffmpeg-kit ─────────────────────────────────────────────────
if [ -d "$FFMPEG_KIT_DIR/android/libs/$ABI" ]; then
    echo "[OK] ffmpeg-kit found for $ABI"
else
    echo ""
    echo "[!] ffmpeg-kit not found. Attempting to download..."
    echo ""
    
    # Create directory structure
    mkdir -p "$FFMPEG_KIT_DIR/android/include"
    mkdir -p "$FFMPEG_KIT_DIR/android/libs/$ABI"
    
    # Try downloading from known sources
    AAR_URL="https://github.com/arthenica/ffmpeg-kit/releases/download/v6.0/ffmpeg-kit-full-6.0-android.aar"
    AAR_FILE="$ANDROID_SDK_DIR/ffmpeg-kit.aar"
    
    if command -v wget &>/dev/null; then
        wget -q --timeout=30 --show-progress "$AAR_URL" -O "$AAR_FILE" 2>/dev/null || true
    elif command -v curl &>/dev/null; then
        curl -sL -o "$AAR_FILE" "$AAR_URL" || true
    fi
    
    if [ -s "$AAR_FILE" ] && [ "$(stat -c%s "$AAR_FILE" 2>/dev/null || echo 0)" -gt 1000 ]; then
        echo "[OK] Downloaded ffmpeg-kit AAR ($(du -h "$AAR_FILE" | cut -f1))"
        echo "  Extracting..."
        unzip -q "$AAR_FILE" -d "$ANDROID_SDK_DIR/ffmpeg-kit-extracted"
        
        # Copy headers
        if [ -d "$ANDROID_SDK_DIR/ffmpeg-kit-extracted/android/$ABI/include" ]; then
            cp -r "$ANDROID_SDK_DIR/ffmpeg-kit-extracted/android/$ABI/include/"* "$FFMPEG_KIT_DIR/android/include/" 2>/dev/null || true
        fi
        
        # Copy libraries
        if [ -d "$ANDROID_SDK_DIR/ffmpeg-kit-extracted/android/$ABI" ]; then
            cp "$ANDROID_SDK_DIR/ffmpeg-kit-extracted/android/$ABI/"*.so "$FFMPEG_KIT_DIR/android/libs/$ABI/" 2>/dev/null || true
        fi
        
        rm -rf "$ANDROID_SDK_DIR/ffmpeg-kit-extracted" "$AAR_FILE"
        
        if [ -d "$FFMPEG_KIT_DIR/android/libs/$ABI" ] && ls "$FFMPEG_KIT_DIR/android/libs/$ABI/"*.so &>/dev/null; then
            echo "[OK] ffmpeg-kit extracted for $ABI"
        else
            echo "[WARN] Extraction incomplete — video support may not work"
        fi
    else
        rm -f "$AAR_FILE"
        echo ""
        echo "╔══════════════════════════════════════════════════════╗"
        echo "║  Manual Setup Required                              ║"
        echo "╠══════════════════════════════════════════════════════╣"
        echo "║  Download ffmpeg-kit-full-6.0-android.aar from:     ║"
        echo "║  https://github.com/arthenica/ffmpeg-kit/releases    ║"
        echo "║                                                      ║"
        echo "║  Then extract:                                       ║"
        echo "║    unzip ffmpeg-kit-full-6.0-android.aar             ║"
        echo "║    cp -r android/<abi>/include/* $FFMPEG_KIT_DIR/android/include/   ║"
        echo "║    cp android/<abi>/*.so $FFMPEG_KIT_DIR/android/libs/<abi>/        ║"
        echo "╚══════════════════════════════════════════════════════╝"
        echo ""
    fi
fi

# ── Build Command ─────────────────────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════════════════"
echo ""
echo "To build for Android:"
echo ""

if [ -d "$FFMPEG_KIT_DIR/android/libs/$ABI" ] && ls "$FFMPEG_KIT_DIR/android/libs/$ABI/"*.so &>/dev/null; then
    echo "  cmake -B build-android \\"
    echo "      -DCMAKE_TOOLCHAIN_FILE=$NDK_DIR/build/cmake/android.toolchain.cmake \\"
    echo "      -DANDROID_ABI=$ABI \\"
    echo "      -DANDROID_PLATFORM=android-26 \\"
    echo "      -DFFMPEG_KIT_DIR=$FFMPEG_KIT_DIR \\"
    echo "      -DCMAKE_BUILD_TYPE=Release"
    echo "  cmake --build build-android --parallel"
else
    echo "  # Without video support (images only):"
    echo "  cmake -B build-android \\"
    echo "      -DCMAKE_TOOLCHAIN_FILE=$NDK_DIR/build/cmake/android.toolchain.cmake \\"
    echo "      -DANDROID_ABI=$ABI \\"
    echo "      -DANDROID_PLATFORM=android-26 \\"
    echo "      -DBATCHPRESS_ENABLE_VIDEO=OFF \\"
    echo "      -DCMAKE_BUILD_TYPE=Release"
    echo "  cmake --build build-android --parallel"
    echo ""
    echo "  # After downloading ffmpeg-kit, re-run with -DFFMPEG_KIT_DIR=$FFMPEG_KIT_DIR"
fi

echo ""
echo "═══════════════════════════════════════════════════════════"
echo ""
echo "  Output: libbatchpress_core.so (+ libbatchpress_jni.so if JNI bridge exists)"
echo ""
