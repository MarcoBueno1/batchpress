#!/bin/bash
# setup_android_ndk.sh
# Downloads Android NDK and ffmpeg-kit, then configures the build environment.
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

# ── Check/Create directories ──────────────────────────────────────────────────
mkdir -p "$FFMPEG_KIT_DIR/android/include"
mkdir -p "$FFMPEG_KIT_DIR/android/libs/$ABI"

# ── Check NDK ─────────────────────────────────────────────────────────────────
if [ ! -f "$NDK_DIR/build/cmake/android.toolchain.cmake" ]; then
    echo "[DOWNLOAD] Android NDK r27c..."
    mkdir -p "$ANDROID_SDK_DIR"
    cd "$ANDROID_SDK_DIR"
    if [ ! -f "android-ndk-r27c-linux.zip" ]; then
        wget -q --show-progress "https://dl.google.com/android/repository/android-ndk-r27c-linux.zip" -O ndk.zip
    fi
    unzip -q ndk.zip
    rm -f ndk.zip
fi
echo "[OK] NDK found: $NDK_DIR"

# ── Check/Download ffmpeg-kit ─────────────────────────────────────────────────
HAS_VIDEO=false
if ls "$FFMPEG_KIT_DIR/android/libs/$ABI/"*.so &>/dev/null 2>&1; then
    echo "[OK] ffmpeg-kit found for $ABI"
    HAS_VIDEO=true
else
    echo ""
    echo "[DOWNLOAD] ffmpeg-kit (62MB)..."
    
    # Download from Grayjay GitLab mirror (official ffmpeg-kit AAR)
    AAR_URL="https://gitlab.futo.org/videostreaming/grayjay/-/raw/315/app/aar/ffmpeg-kit-full-6.0-2.LTS.aar"
    AAR_FILE="$ANDROID_SDK_DIR/ffmpeg-kit.aar"
    
    if [ ! -f "$AAR_FILE" ] || [ ! -s "$AAR_FILE" ]; then
        cd "$ANDROID_SDK_DIR"
        wget -q --show-progress "$AAR_URL" -O "$AAR_FILE"
    fi
    
    if [ -s "$AAR_FILE" ]; then
        echo "[EXTRACT] ffmpeg-kit..."
        EXTRACT_DIR="$ANDROID_SDK_DIR/ffmpeg-kit-extracted"
        rm -rf "$EXTRACT_DIR"
        unzip -q "$AAR_FILE" -d "$EXTRACT_DIR"
        
        # Copy native libraries
        if [ -d "$EXTRACT_DIR/jni/$ABI" ]; then
            cp "$EXTRACT_DIR/jni/$ABI/"*.so "$FFMPEG_KIT_DIR/android/libs/$ABI/"
            echo "[OK] Native libs for $ABI: $(ls "$FFMPEG_KIT_DIR/android/libs/$ABI/"*.so | wc -l) files"
        fi
        
        # Download and copy FFmpeg headers
        if [ ! -d "$FFMPEG_KIT_DIR/android/include/libavcodec" ]; then
            echo "[DOWNLOAD] FFmpeg headers..."
            SRC_FILE="$ANDROID_SDK_DIR/ffmpeg-src.tar.gz"
            if [ ! -f "$SRC_FILE" ]; then
                wget -q --show-progress "https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n6.0.tar.gz" -O "$SRC_FILE"
            fi
            tar -xzf "$SRC_FILE" -C "$ANDROID_SDK_DIR" FFmpeg-n6.0/libavcodec FFmpeg-n6.0/libavformat FFmpeg-n6.0/libavutil FFmpeg-n6.0/libswscale FFmpeg-n6.0/libswresample 2>/dev/null || true
            
            for dir in libavcodec libavformat libavutil libswscale libswresample; do
                mkdir -p "$FFMPEG_KIT_DIR/android/include/$dir"
                cp "$ANDROID_SDK_DIR/FFmpeg-n6.0/$dir/"*.h "$FFMPEG_KIT_DIR/android/include/$dir/" 2>/dev/null || true
            done
            
            # Create generated header
            cat > "$FFMPEG_KIT_DIR/android/include/libavutil/avconfig.h" << 'AVCONFIG'
#ifndef AVUTIL_AVCONFIG_H
#define AVUTIL_AVCONFIG_H
#define AV_HAVE_BIGENDIAN 0
#define AV_HAVE_FAST_UNALIGNED 1
#endif
AVCONFIG
            echo "[OK] FFmpeg headers copied"
        fi
        
        rm -rf "$EXTRACT_DIR"
        HAS_VIDEO=true
    else
        echo "[WARN] Download failed — video support will not be available"
    fi
fi

# ── Build Command ─────────────────────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════════════════"
echo ""

if [ "$HAS_VIDEO" = true ]; then
    echo "  # Build WITH video support:"
    echo "  cmake -B build-android \\"
    echo "      -DCMAKE_TOOLCHAIN_FILE=$NDK_DIR/build/cmake/android.toolchain.cmake \\"
    echo "      -DANDROID_ABI=$ABI \\"
    echo "      -DANDROID_PLATFORM=android-26 \\"
    echo "      -DFFMPEG_KIT_DIR=$FFMPEG_KIT_DIR \\"
    echo "      -DCMAKE_BUILD_TYPE=Release"
    echo "  cmake --build build-android --parallel"
else
    echo "  # Build WITHOUT video support:"
    echo "  cmake -B build-android \\"
    echo "      -DCMAKE_TOOLCHAIN_FILE=$NDK_DIR/build/cmake/android.toolchain.cmake \\"
    echo "      -DANDROID_ABI=$ABI \\"
    echo "      -DANDROID_PLATFORM=android-26 \\"
    echo "      -DBATCHPRESS_ENABLE_VIDEO=OFF \\"
    echo "      -DCMAKE_BUILD_TYPE=Release"
    echo "  cmake --build build-android --parallel"
fi

echo ""
echo "═══════════════════════════════════════════════════════════"
echo ""
echo "  Output: libbatchpress_core.so (+ libbatchpress_jni.so)"
echo ""
