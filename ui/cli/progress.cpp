// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.mario@gmail.com>
 *
 * This file is part of batchpress.
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

#include "progress.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace cli {

ProgressBar::ProgressBar(uint32_t total, uint32_t bar_width)
    : total_(total), bar_width_(bar_width),
      start_(std::chrono::steady_clock::now())
{
    // Hide cursor during progress
    std::cout << "\033[?25l" << std::flush;
}

void ProgressBar::tick(bool success) {
    std::lock_guard lock(mu_);
    ++current_;
    if (!success) ++failures_;
    render_locked();
}

void ProgressBar::finish() {
    std::lock_guard lock(mu_);
    render_locked();
    // Restore cursor, move to next line
    std::cout << "\033[?25h\n" << std::flush;
}

void ProgressBar::render_locked() {
    using namespace std::chrono;

    double pct    = total_ > 0 ? static_cast<double>(current_) / total_ : 0.0;
    uint32_t fill = static_cast<uint32_t>(std::round(pct * bar_width_));
    fill = std::min(fill, bar_width_);

    auto now     = steady_clock::now();
    double secs  = duration<double>(now - start_).count();
    double rate  = secs > 0.0 ? current_ / secs : 0.0;
    double eta   = (rate > 0.0 && current_ < total_)
                    ? (total_ - current_) / rate : 0.0;

    // Build bar string
    std::ostringstream bar;
    bar << "\r\033[K"; // carriage return + clear line

    // Colour: green if no failures, yellow if some
    const char* colour = failures_ > 0 ? "\033[33m" : "\033[32m";
    bar << colour << "[";
    for (uint32_t i = 0; i < bar_width_; ++i)
        bar << (i < fill ? "█" : "░");
    bar << "]\033[0m";

    // Percentage
    bar << " " << std::setw(3) << static_cast<int>(pct * 100) << "%";

    // Count
    bar << " — " << current_ << "/" << total_;

    // Rate (videos per second or images per second)
    if (rate >= 1.0)
        bar << " — " << std::fixed << std::setprecision(0) << rate << " files/s";
    else if (rate > 0.0)
        bar << " — " << std::fixed << std::setprecision(1) << (rate * 60) << " files/min";

    // ETA
    if (current_ < total_ && eta > 0.0) {
        bar << " — ETA ";
        if (eta >= 3600.0)
            bar << std::setprecision(0) << eta / 3600.0 << "h";
        else if (eta >= 60.0)
            bar << std::setprecision(0) << eta / 60.0 << "m";
        else
            bar << std::setprecision(1) << eta << "s";
    }

    // Failures badge
    if (failures_ > 0)
        bar << " \033[31m[" << failures_ << " err]\033[0m";

    // Success indicator
    if (current_ == total_ && failures_ == 0)
        bar << " \033[32m✓\033[0m";
    else if (current_ == total_ && failures_ > 0)
        bar << " \033[33m!\033[0m";

    std::cout << bar.str() << std::flush;
}

} // namespace cli
