// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.mario@gmail.com>
 *
 * This file is part of batchpress — CLI scan reporter.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "scan_report.hpp"
#include <batchpress/scanner.hpp>
#include <batchpress/types.hpp>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace cli {

// ── Formatting helpers ────────────────────────────────────────────────────────

static std::string human_bytes(uint64_t b) {
    const char* u[] = {"B","KB","MB","GB","TB"};
    double v = static_cast<double>(b);
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << v << " " << u[i];
    return ss.str();
}

static std::string stars(int n) {
    std::string s;
    for (int i = 0; i < 3; ++i)
        s += (i < n) ? "\033[33m★\033[0m" : "\033[90m☆\033[0m";
    return s;
}

static std::string bar_pct(double pct, int width = 20) {
    int fill = static_cast<int>(pct / 100.0 * width);
    fill = std::max(0, std::min(fill, width));
    std::string s = "\033[32m";
    for (int i = 0; i < width; ++i) s += (i < fill) ? "█" : "░";
    s += "\033[0m";
    return s;
}

// ── print_scan_report ─────────────────────────────────────────────────────────

void print_scan_report(const batchpress::ScanReport& r, bool verbose) {

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║               batchpress — Scan Report                    ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";

    // ── Summary header ────────────────────────────────────────────────────
    std::cout << "  Scanned : \033[1m" << r.total_images << "\033[0m images"
              << "  │  Total size: \033[1m" << human_bytes(r.total_bytes) << "\033[0m"
              << "  │  Time: " << std::fixed << std::setprecision(1)
              << r.elapsed_sec << "s\n\n";

    // ── Per-directory results ─────────────────────────────────────────────
    for (const auto& dir : r.directories) {

        if (dir.image_count == 0) continue;

        // Directory header
        std::cout << "\033[36m┌─ " << dir.directory.string()
                  << " \033[0m\033[90m(" << dir.image_count << " images  │  "
                  << human_bytes(dir.total_bytes) << ")\033[0m\n";

        if (dir.best_projected_bytes == 0) {
            std::cout << "\033[90m│  (no candidate improved this directory)\033[0m\n\n";
            continue;
        }

        int64_t saved = dir.bytes_saved();

        // Best suggestion line
        std::cout << "│  \033[1mBest:\033[0m  \033[32m"
                  << dir.best_candidate.label() << "\033[0m\n";

        // Size bar
        std::cout << "│  " << bar_pct(dir.best_savings_pct)
                  << "  " << human_bytes(dir.total_bytes)
                  << " \033[90m→\033[0m \033[32m" << human_bytes(dir.best_projected_bytes) << "\033[0m"
                  << "  \033[33msave " << std::fixed << std::setprecision(1)
                  << dir.best_savings_pct << "%\033[0m"
                  << "  " << stars(dir.stars()) << "\n";

        std::cout << "│  Free up: \033[32m" << human_bytes(static_cast<uint64_t>(saved))
                  << "\033[0m\n";

        // Verbose: show all tested candidates
        if (verbose && dir.candidates.size() > 1) {
            std::cout << "│\n│  \033[90mAll candidates tested:\033[0m\n";
            size_t max_show = std::min(dir.candidates.size(), size_t(8));
            for (size_t i = 0; i < max_show; ++i) {
                const auto& cr = dir.candidates[i];
                std::cout << "│    " << std::setw(2) << (i+1) << ".  "
                          << std::left << std::setw(28) << cr.candidate.label()
                          << std::right
                          << "  " << human_bytes(cr.projected_bytes)
                          << "  (" << std::fixed << std::setprecision(1)
                          << cr.savings_pct << "% saved"
                          << "  samples=" << cr.samples_used << ")\n";
            }
        }

        std::cout << "\033[36m└\033[0m\n\n";
    }

    // ── Global totals ─────────────────────────────────────────────────────
    int64_t total_saved = r.bytes_saved();

    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║  TOTAL PROJECTION                                        ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════╣\n";

    auto row = [](const std::string& label, const std::string& value,
                  const char* colour = "\033[0m") {
        std::cout << "║  " << colour
                  << std::left  << std::setw(22) << label
                  << "\033[0m"
                  << std::right << std::setw(32) << value
                  << "  ║\n";
    };

    row("Current total size",  human_bytes(r.total_bytes));
    row("Projected size",      human_bytes(r.projected_bytes),      "\033[35m");
    row("Space freed",
        human_bytes(static_cast<uint64_t>(total_saved > 0 ? total_saved : 0))
        + "  (" + [&]{
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(1) << r.savings_pct() << "%";
            return ss.str();
        }() + ")",
        "\033[32m");

    // Disk free context
    uint64_t disk_free = batchpress::disk_free_bytes(r.root_dir);
    row("Disk free now",   human_bytes(disk_free));
    row("Disk free after", human_bytes(disk_free + static_cast<uint64_t>(
                                total_saved > 0 ? total_saved : 0)), "\033[32m");

    std::cout << "╠══════════════════════════════════════════════════════════╣\n";

    // Global best candidate
    batchpress::ScanCandidate global_best = r.global_best_candidate();
    std::cout << "║  \033[1mGlobal best config:\033[0m  \033[32m"
              << std::left << std::setw(38) << global_best.label()
              << "\033[0m║\n";

    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << "║  \033[33mTo apply:\033[0m                                                ║\n";
    std::cout << "║  \033[36m" << std::left << std::setw(56)
              << r.suggested_command() << "\033[0m║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";
}

} // namespace cli
