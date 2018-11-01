// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.marco@gmail.com>
 *
 * This file is part of batchpress — CLI video scan reporter.
 */

#include "video_scan_report.hpp"
#include <batchpress/types.hpp>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace cli {

static std::string human_bytes(uint64_t b) {
    const char* u[] = {"B","KB","MB","GB","TB"};
    double v = static_cast<double>(b);
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << v << " " << u[i];
    return ss.str();
}

static std::string human_duration(uint64_t sec) {
    if (sec < 60)   return std::to_string(sec) + "s";
    if (sec < 3600) return std::to_string(sec/60) + "m " + std::to_string(sec%60) + "s";
    return std::to_string(sec/3600) + "h " + std::to_string((sec%3600)/60) + "m";
}

static std::string stars(int n) {
    std::string s;
    for (int i = 0; i < 3; ++i)
        s += (i < n) ? "\033[33m★\033[0m" : "\033[90m☆\033[0m";
    return s;
}

static std::string codec_name(batchpress::VideoCodec vc) {
    switch (vc) {
        case batchpress::VideoCodec::H265: return "\033[32mH.265\033[0m";
        case batchpress::VideoCodec::H264: return "\033[33mH.264\033[0m";
        case batchpress::VideoCodec::VP9:  return "\033[36mVP9\033[0m";
        default: return "Auto";
    }
}

void print_video_scan_report(const batchpress::VideoScanReport& r, bool verbose) {

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║            batchpress — Video Scan Report                 ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";

    // ── Codec availability ────────────────────────────────────────────────
    std::cout << "  Codecs available on this system:\n";
    std::cout << "    H.265 (HEVC)  " << (r.caps.has_h265 ? "\033[32m✓\033[0m" : "\033[31m✗\033[0m") << "\n";
    std::cout << "    H.264 (AVC)   " << (r.caps.has_h264 ? "\033[32m✓\033[0m" : "\033[31m✗\033[0m") << "\n";
    std::cout << "    VP9           " << (r.caps.has_vp9  ? "\033[32m✓\033[0m" : "\033[31m✗\033[0m") << "\n";
    std::cout << "    AAC           " << (r.caps.has_aac  ? "\033[32m✓\033[0m" : "\033[31m✗\033[0m") << "\n";
    std::cout << "    Opus          " << (r.caps.has_opus ? "\033[32m✓\033[0m" : "\033[31m✗\033[0m") << "\n\n";

    std::cout << "  Scanned : \033[1m" << r.total_videos << "\033[0m videos"
              << "  │  Total: \033[1m" << human_bytes(r.total_bytes) << "\033[0m"
              << "  │  Time: " << std::fixed << std::setprecision(1)
              << r.elapsed_sec << "s\n\n";

    // ── Per-directory results ─────────────────────────────────────────────
    for (const auto& dir : r.directories) {
        if (dir.video_count == 0) continue;

        std::cout << "\033[36m┌─ " << dir.directory.string()
                  << "\033[0m\033[90m  (" << dir.video_count << " videos"
                  << "  │  " << human_bytes(dir.total_bytes)
                  << "  │  " << human_duration(dir.total_duration_sec) << ")\033[0m\n";

        std::cout << "│  Codec:    " << codec_name(dir.suggested_codec)
                  << "  CRF " << dir.suggested_crf << "\n";
        std::cout << "│  Result:   "
                  << human_bytes(dir.total_bytes)
                  << " \033[90m→\033[0m \033[32m"
                  << human_bytes(dir.projected_bytes) << "\033[0m"
                  << "  \033[33m" << std::fixed << std::setprecision(1)
                  << dir.savings_pct << "% saved\033[0m"
                  << "  " << stars(dir.stars()) << "\n";
        std::cout << "│  Free up:  \033[32m"
                  << human_bytes(static_cast<uint64_t>(
                         dir.bytes_saved() > 0 ? dir.bytes_saved() : 0))
                  << "\033[0m\n";

        if (verbose) {
            std::cout << "│  Command:  \033[90m"
                      << dir.suggested_command() << "\033[0m\n";
        }

        std::cout << "\033[36m└\033[0m\n\n";
    }

    // ── Global totals ─────────────────────────────────────────────────────
    int64_t saved = r.bytes_saved();

    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  VIDEO TOTAL PROJECTION                                  ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════╣\n";

    auto row = [](const std::string& label, const std::string& value,
                  const char* colour = "\033[0m") {
        std::cout << "║  " << colour
                  << std::left  << std::setw(22) << label
                  << "\033[0m"
                  << std::right << std::setw(32) << value
                  << "  ║\n";
    };

    row("Current total",    human_bytes(r.total_bytes));
    row("Projected size",   human_bytes(r.projected_bytes),   "\033[35m");

    std::ostringstream sv;
    sv << human_bytes(static_cast<uint64_t>(saved > 0 ? saved : 0))
       << "  (" << std::fixed << std::setprecision(1) << r.savings_pct() << "%)";
    row("Space freed", sv.str(), "\033[32m");

    uint64_t free_now   = batchpress::disk_free_bytes(r.root_dir);
    uint64_t free_after = free_now + static_cast<uint64_t>(saved > 0 ? saved : 0);
    row("Disk free now",    human_bytes(free_now));
    row("Disk free after",  human_bytes(free_after), "\033[32m");

    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << "║  \033[33mTo apply:\033[0m                                                ║\n";
    std::cout << "║  \033[36m" << std::left << std::setw(56)
              << r.suggested_command() << "\033[0m║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";
}

} // namespace cli
