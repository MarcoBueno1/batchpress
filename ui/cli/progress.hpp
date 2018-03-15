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

#pragma once

#include <cstdint>
#include <string>
#include <mutex>
#include <chrono>

namespace cli {

/**
 * @brief Thread-safe terminal progress bar.
 *
 * Renders a live updating bar to stdout using ANSI escape codes.
 * Safe to call tick() from multiple threads simultaneously.
 *
 * Example output:
 *   [████████████░░░░░░░░] 60% — 600/1000 — 312 img/s — ETA 1.3s
 */
class ProgressBar {
public:
    explicit ProgressBar(uint32_t total, uint32_t bar_width = 40);

    /// Increment counter by 1 and redraw. Thread-safe.
    void tick(bool success = true);

    /// Force a final redraw and print newline.
    void finish();

    uint32_t current()  const noexcept { return current_; }
    uint32_t failures() const noexcept { return failures_; }

private:
    void render_locked();

    uint32_t    total_;
    uint32_t    bar_width_;
    uint32_t    current_  = 0;
    uint32_t    failures_ = 0;
    std::mutex  mu_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace cli
