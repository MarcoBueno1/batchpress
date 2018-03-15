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

#include "batchpress/types.hpp"
#include <algorithm>
#include <stdexcept>
#include <cctype>
#include <cmath>

namespace batchpress {

// ── ImageFormat ───────────────────────────────────────────────────────────────

ImageFormat parse_format(const std::string& raw) {
    std::string s = raw;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s == "same" || s.empty()) return ImageFormat::Same;
    if (s == "jpg"  || s == "jpeg") return ImageFormat::JPEG;
    if (s == "png")  return ImageFormat::PNG;
    if (s == "bmp")  return ImageFormat::BMP;
    if (s == "webp") return ImageFormat::WebP;
    throw std::invalid_argument("Unknown format: " + raw
        + ". Valid: jpg, png, bmp, webp, same");
}

std::string format_extension(ImageFormat fmt) {
    switch (fmt) {
        case ImageFormat::JPEG: return ".jpg";
        case ImageFormat::PNG:  return ".png";
        case ImageFormat::BMP:  return ".bmp";
        case ImageFormat::WebP: return ".webp";
        default:                return "";
    }
}

// ── ResizeSpec ────────────────────────────────────────────────────────────────

ResizeSpec parse_resize(const std::string& s) {
    if (s.empty()) return {};
    ResizeSpec spec;

    if (s.back() == '%') {
        float pct = std::stof(s.substr(0, s.size() - 1));
        if (pct <= 0.0f || pct > 10000.0f)
            throw std::invalid_argument("Resize percent out of range: " + s);
        spec.mode = ResizeSpec::Mode::Percent;
        spec.pct  = pct / 100.0f;
        return spec;
    }

    std::string dims = s;
    if (s.rfind("fit:", 0) == 0) {
        dims      = s.substr(4);
        spec.mode = ResizeSpec::Mode::Fit;
    } else {
        spec.mode = ResizeSpec::Mode::Exact;
    }

    auto x = dims.find('x');
    if (x == std::string::npos)
        throw std::invalid_argument("Invalid resize spec: " + s
            + ". Expected: 1920x1080 | 50% | fit:1280x720");

    spec.width  = static_cast<uint32_t>(std::stoul(dims.substr(0, x)));
    spec.height = static_cast<uint32_t>(std::stoul(dims.substr(x + 1)));
    if (spec.width == 0 || spec.height == 0)
        throw std::invalid_argument("Resize dimensions must be > 0");

    return spec;
}

std::pair<uint32_t, uint32_t>
ResizeSpec::compute(uint32_t w, uint32_t h) const noexcept {
    switch (mode) {
        case Mode::None:    return {w, h};
        case Mode::Exact:   return {width, height};
        case Mode::Percent: {
            auto nw = static_cast<uint32_t>(std::round(w * pct));
            auto nh = static_cast<uint32_t>(std::round(h * pct));
            return {std::max(1u, nw), std::max(1u, nh)};
        }
        case Mode::Fit: {
            float scale = std::min(
                static_cast<float>(width)  / static_cast<float>(w),
                static_cast<float>(height) / static_cast<float>(h));
            auto nw = static_cast<uint32_t>(std::round(w * scale));
            auto nh = static_cast<uint32_t>(std::round(h * scale));
            return {std::max(1u, nw), std::max(1u, nh)};
        }
    }
    return {w, h};
}

// ── BatchReport ───────────────────────────────────────────────────────────────

double BatchReport::throughput() const noexcept {
    return elapsed_sec > 0.0 ? succeeded / elapsed_sec : 0.0;
}

int64_t BatchReport::bytes_saved() const noexcept {
    return static_cast<int64_t>(input_bytes_total)
         - static_cast<int64_t>(output_bytes_total);
}

double BatchReport::savings_pct() const noexcept {
    if (input_bytes_total == 0) return 0.0;
    return 100.0 * (1.0 - static_cast<double>(output_bytes_total)
                           / static_cast<double>(input_bytes_total));
}

// ── disk_free_bytes ───────────────────────────────────────────────────────────

uint64_t disk_free_bytes(const fs::path& path) noexcept {
    try {
        fs::path check = fs::exists(path) ? path : path.parent_path();
        if (check.empty()) check = fs::current_path();
        return static_cast<uint64_t>(fs::space(check).available);
    } catch (...) {
        return 0;
    }
}

} // namespace batchpress
