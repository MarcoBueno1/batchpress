#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.marco@gmail.com>
#
# generate_test_videos.sh
# Generates synthetic test videos in multiple formats using ffmpeg.
# Each video is short (3-10s) but covers different resolutions,
# codecs, containers and audio types.
#
# Requirements: ffmpeg (apt install ffmpeg)

set -e

BASE="$(dirname "$0")/testdata/videos"
mkdir -p "$BASE/mp4" "$BASE/mov" "$BASE/subdir"

FF="ffmpeg -y -loglevel error"

echo ""
echo "Generating test videos → $BASE"
echo ""

# ── Helper: colourful moving bars (CPU-light test source) ─────────────────────
# lavfi testsrc2 generates a standard SMPTE-style colour test card with motion.

gen_video() {
    local out="$1"
    local res="$2"      # e.g. 1920x1080
    local dur="$3"      # seconds
    local vcodec="$4"   # libx264, libx265, libvpx-vp9, mpeg4
    local atype="$5"    # sine, speech, silent
    local extra="${6:-}"

    local w="${res%x*}"
    local h="${res#*x}"

    # Audio source
    local audio_filter=""
    case "$atype" in
        sine)
            # Pure tone — classified as Music by batchpress
            audio_filter="-f lavfi -i \"sine=frequency=440:sample_rate=44100:duration=$dur\""
            ;;
        speech)
            # Filtered noise in speech frequency band — classified as Speech
            audio_filter="-f lavfi -i \"anoisesrc=color=pink:sample_rate=16000:duration=$dur,highpass=f=80,lowpass=f=3400\""
            ;;
        silent)
            # True silence — batchpress should remove audio track
            audio_filter="-f lavfi -i \"anullsrc=channel_layout=stereo:sample_rate=44100\" -t $dur"
            ;;
    esac

    eval "$FF \
        -f lavfi -i \"testsrc2=size=${w}x${h}:rate=30:duration=$dur\" \
        $audio_filter \
        -c:v $vcodec \
        -t $dur \
        $extra \
        \"$out\""

    local size_kb=$(du -k "$out" | cut -f1)
    echo "  ✓ $(basename $(dirname $out))/$(basename $out)  ${res}  ${dur}s  ${size_kb} KB"
}

# ── MP4 files (H.264, most common) ───────────────────────────────────────────

gen_video "$BASE/mp4/video_4k_h264_sine.mp4"       "3840x2160"  5  "libx264"      "sine"   "-preset ultrafast -crf 23"
gen_video "$BASE/mp4/video_1080p_h264_speech.mp4"  "1920x1080"  8  "libx264"      "speech" "-preset ultrafast -crf 23"
gen_video "$BASE/mp4/video_720p_h264_silent.mp4"   "1280x720"   5  "libx264"      "silent" "-preset ultrafast -crf 23"
gen_video "$BASE/mp4/video_480p_h264_sine.mp4"     "854x480"    4  "libx264"      "sine"   "-preset ultrafast -crf 28"
gen_video "$BASE/mp4/video_360p_h264_speech.mp4"   "640x360"    3  "libx264"      "speech" "-preset ultrafast -crf 28"

# ── MOV files (QuickTime container, common on macOS/iOS) ──────────────────────

gen_video "$BASE/mov/video_1080p_mov_sine.mov"     "1920x1080"  5  "libx264"      "sine"   "-preset ultrafast -crf 23"
gen_video "$BASE/mov/video_720p_mov_speech.mov"    "1280x720"   4  "libx264"      "speech" "-preset ultrafast -crf 26"

# ── WebM (VP9) ────────────────────────────────────────────────────────────────
# Check if VP9 encoder available

if ffmpeg -encoders 2>/dev/null | grep -q libvpx-vp9; then
    gen_video "$BASE/mp4/video_720p_vp9.webm"      "1280x720"   4  "libvpx-vp9"   "sine"   "-crf 33 -b:v 0 -deadline realtime -cpu-used 8"
else
    echo "  ⚠ libvpx-vp9 not available — skipping WebM"
fi

# ── Subdir: test recursive traversal ─────────────────────────────────────────

gen_video "$BASE/subdir/nested_clip.mp4"           "640x360"    3  "libx264"      "speech" "-preset ultrafast -crf 30"

# ── Summary ───────────────────────────────────────────────────────────────────

total=$(find "$BASE" -type f | wc -l)
size_mb=$(du -sm "$BASE" | cut -f1)
echo ""
echo "✓ $total video files  ~${size_mb} MB total"
echo ""
