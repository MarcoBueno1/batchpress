#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.mario@gmail.com>
#
# run_tests.sh — Generate testdata and run all batchpress tests
#
# Usage:
#   chmod +x tests/run_tests.sh
#   bash tests/run_tests.sh [--no-generate] [--no-build]

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TESTS="$ROOT/tests"
BUILD="$ROOT/build"

GENERATE=1
BUILD_PROJ=1

for arg in "$@"; do
    case "$arg" in
        --no-generate) GENERATE=0 ;;
        --no-build)    BUILD_PROJ=0 ;;
    esac
done

echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║          batchpress — Test Runner                   ║"
echo "╚══════════════════════════════════════════════════════╝"
echo ""

# ── Step 1: Generate testdata ─────────────────────────────────────────────────

if [ "$GENERATE" -eq 1 ]; then
    echo "── Step 1: Generating test images ──────────────────────"
    python3 "$TESTS/generate_test_images.py"

    echo ""
    echo "── Step 2: Generating test videos ──────────────────────"
    bash "$TESTS/generate_test_videos.sh"
else
    echo "── Skipping testdata generation (--no-generate) ────────"
fi

# ── Step 2: Build ─────────────────────────────────────────────────────────────

if [ "$BUILD_PROJ" -eq 1 ]; then
    echo ""
    echo "── Step 3: Building batchpress ─────────────────────────"

    cmake -B "$BUILD" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=ON \
        -G Ninja \
        "$ROOT" 2>&1 | tail -5

    cmake --build "$BUILD" --parallel

    echo "Build OK"
fi

# ── Step 3: Run tests ─────────────────────────────────────────────────────────

echo ""
echo "── Step 4: Running tests ────────────────────────────────"
echo ""

cd "$BUILD"
ctest --output-on-failure --test-dir "$BUILD"

echo ""
echo "✓ All tests passed."
echo ""

# ── Step 4: Summary of generated files ───────────────────────────────────────

echo "── Testdata summary ─────────────────────────────────────"
echo ""
if [ -d "$TESTS/testdata/images" ]; then
    img_count=$(find "$TESTS/testdata/images" -type f | wc -l)
    img_size=$(du -sh "$TESTS/testdata/images" | cut -f1)
    echo "  Images: $img_count files  ($img_size)"
fi
if [ -d "$TESTS/testdata/videos" ]; then
    vid_count=$(find "$TESTS/testdata/videos" -type f | wc -l)
    vid_size=$(du -sh "$TESTS/testdata/videos" | cut -f1)
    echo "  Videos: $vid_count files  ($vid_size)"
fi
echo ""
