// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.marco@gmail.com>
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

/*
 * DESIGN RULE: No <iostream>, no UI code. All feedback via ScanConfig::on_progress.
 */

#include "batchpress/scanner.hpp"
#include "batchpress/thread_pool.hpp"

#ifdef BATCHPRESS_HAS_WEBP
#include <webp/encode.h>
#endif

// ── stb single-file libraries ─────────────────────────────────────────────────
// STB_IMAGE_STATIC gives all stb symbols internal linkage — no conflicts
// when stb is included in multiple translation units.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STB_IMAGE_WRITE_STATIC
#define STBIRDEF static          // makes stb_image_resize2 symbols static too
#define STBI_FAILURE_USERMSG

#include "../third_party/stb_image.h"
#include "../third_party/stb_image_write.h"
#include "../third_party/stb_image_resize2.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace batchpress {

// ── Supported extensions (shared with processor) ─────────────────────────────

static const std::vector<std::string> SCAN_EXT = {
    ".jpg", ".jpeg", ".png", ".bmp", ".tga", ".hdr", ".pic", ".pnm"
};

static bool scan_supported(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    for (const auto& e : SCAN_EXT) if (ext == e) return true;
    return false;
}

// ── ScanCandidate::label() ────────────────────────────────────────────────────

std::string ScanCandidate::label() const {
    std::ostringstream ss;

    switch (format) {
        case ImageFormat::JPEG: ss << "JPEG"; break;
        case ImageFormat::PNG:  ss << "PNG";  break;
        case ImageFormat::BMP:  ss << "BMP";  break;
        case ImageFormat::WebP: ss << "WebP"; break;
        default:                ss << "Same"; break;
    }

    if (format == ImageFormat::JPEG || format == ImageFormat::WebP)
        ss << " q" << quality;

    if (resize.active()) {
        switch (resize.mode) {
            case ResizeSpec::Mode::Exact:
                ss << " " << resize.width << "x" << resize.height; break;
            case ResizeSpec::Mode::Fit:
                ss << " fit:" << resize.width << "x" << resize.height; break;
            case ResizeSpec::Mode::Percent:
                ss << " " << static_cast<int>(resize.pct * 100) << "%"; break;
            default: break;
        }
    }

    return ss.str();
}

// ── Default candidates ────────────────────────────────────────────────────────

std::vector<ScanCandidate> default_scan_candidates() {
    std::vector<ScanCandidate> v;

    // WebP at different quality levels
    for (int q : {85, 75, 60}) {
        ScanCandidate c;
        c.format  = ImageFormat::WebP;
        c.quality = q;
        v.push_back(c); // no resize

        // + fit:1920x1080
        c.resize = parse_resize("fit:1920x1080");
        v.push_back(c);

        // + 50%
        c.resize = parse_resize("50%");
        v.push_back(c);
    }

    // JPEG at different quality levels
    for (int q : {85, 75}) {
        ScanCandidate c;
        c.format  = ImageFormat::JPEG;
        c.quality = q;
        v.push_back(c);

        c.resize = parse_resize("fit:1920x1080");
        v.push_back(c);
    }

    // PNG lossless (good for screenshots)
    {
        ScanCandidate c;
        c.format = ImageFormat::PNG;
        v.push_back(c);
    }

    // Keep original format, just resize
    {
        ScanCandidate c;
        c.format = ImageFormat::Same;
        c.resize = parse_resize("50%");
        v.push_back(c);

        c.resize = parse_resize("fit:1920x1080");
        v.push_back(c);
    }

    return v;
}

// ── ScanReport helpers ────────────────────────────────────────────────────────

int64_t ScanReport::bytes_saved() const noexcept {
    return static_cast<int64_t>(total_bytes)
         - static_cast<int64_t>(projected_bytes);
}

double ScanReport::savings_pct() const noexcept {
    if (total_bytes == 0) return 0.0;
    return 100.0 * (1.0 - static_cast<double>(projected_bytes)
                           / static_cast<double>(total_bytes));
}

ScanCandidate ScanReport::global_best_candidate() const {
    // Find the candidate label that saves most bytes across all dirs combined
    std::map<std::string, int64_t> savings_by_label;

    for (const auto& dir : directories) {
        for (const auto& cr : dir.candidates) {
            int64_t saved = static_cast<int64_t>(dir.total_bytes)
                          - static_cast<int64_t>(cr.projected_bytes);
            savings_by_label[cr.candidate.label()] += saved;
        }
    }

    std::string best_label;
    int64_t     best_saved = -1;
    for (const auto& [label, saved] : savings_by_label) {
        if (saved > best_saved) {
            best_saved = saved;
            best_label = label;
        }
    }

    // Return the candidate object matching the best label
    for (const auto& dir : directories)
        for (const auto& cr : dir.candidates)
            if (cr.candidate.label() == best_label)
                return cr.candidate;

    return {};
}

std::string ScanReport::suggested_command(const std::string& exe) const {
    ScanCandidate best = global_best_candidate();
    std::ostringstream ss;
    ss << exe << " --input \"" << root_dir.string() << "\"";

    if (best.format != ImageFormat::Same) {
        std::string fmt;
        switch (best.format) {
            case ImageFormat::JPEG: fmt = "jpg";  break;
            case ImageFormat::PNG:  fmt = "png";  break;
            case ImageFormat::WebP: fmt = "webp"; break;
            case ImageFormat::BMP:  fmt = "bmp";  break;
            default: break;
        }
        if (!fmt.empty()) ss << " --format " << fmt;
    }

    if (best.format == ImageFormat::JPEG || best.format == ImageFormat::WebP)
        ss << " --quality " << best.quality;

    if (best.resize.active()) {
        switch (best.resize.mode) {
            case ResizeSpec::Mode::Exact:
                ss << " --resize " << best.resize.width << "x" << best.resize.height;
                break;
            case ResizeSpec::Mode::Fit:
                ss << " --resize fit:" << best.resize.width << "x" << best.resize.height;
                break;
            case ResizeSpec::Mode::Percent:
                ss << " --resize " << static_cast<int>(best.resize.pct * 100) << "%";
                break;
            default: break;
        }
    }

    return ss.str();
}

// ── stb write-to-memory callback ─────────────────────────────────────────────

static void scan_write_cb(void* ctx, void* data, int size) {
    auto* buf = static_cast<std::vector<uint8_t>*>(ctx);
    auto* ptr = static_cast<uint8_t*>(data);
    buf->insert(buf->end(), ptr, ptr + size);
}

// ── Read image metadata without full decode ───────────────────────────────────

static ImageMeta read_meta(const fs::path& path) {
    ImageMeta m;
    m.path       = path;
    m.file_bytes = fs::exists(path) ? fs::file_size(path) : 0;
    m.extension  = path.extension().string();
    std::transform(m.extension.begin(), m.extension.end(),
                   m.extension.begin(), ::tolower);

    // stbi_info reads only the header — no pixel decode, very fast
    if (!stbi_info(path.string().c_str(),
                   reinterpret_cast<int*>(&m.width),
                   reinterpret_cast<int*>(&m.height),
                   &m.channels))
    {
        m.width = m.height = 0; // mark as unreadable
    }

    return m;
}

// ── Encode one image sample for a given candidate ─────────────────────────────
// Returns estimated encoded size in bytes, or 0 on failure.

static uint64_t encode_sample(const ImageMeta& meta,
                               const ScanCandidate& candidate)
{
    if (meta.width == 0) return 0;

    // Decode pixels
    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load(
        meta.path.string().c_str(), &w, &h, &ch, 0);
    if (!pixels) return 0;

    struct Guard {
        unsigned char* p;
        explicit Guard(unsigned char* px) : p(px) {}
        ~Guard() { if (p) stbi_image_free(p); }
        Guard(const Guard&) = delete;
    } guard(pixels);

    // Resize if requested
    unsigned char* write_px = pixels;
    int dst_w = w, dst_h = h;
    std::vector<unsigned char> resized_buf;

    if (candidate.resize.active()) {
        auto [nw, nh] = candidate.resize.compute(
            static_cast<uint32_t>(w),
            static_cast<uint32_t>(h));
        dst_w = static_cast<int>(nw);
        dst_h = static_cast<int>(nh);
        resized_buf.resize(static_cast<size_t>(dst_w * dst_h * ch));

        if (!stbir_resize_uint8_linear(
                pixels, w, h, 0,
                resized_buf.data(), dst_w, dst_h, 0,
                static_cast<stbir_pixel_layout>(ch)))
            return 0;

        write_px = resized_buf.data();
    }

    // Determine output extension
    std::string ext;
    if (candidate.format != ImageFormat::Same)
        ext = format_extension(candidate.format);
    else
        ext = meta.extension;

    // Encode to memory
    std::vector<uint8_t> buf;
    buf.reserve(static_cast<size_t>(dst_w * dst_h * ch / 4));

    int ok = 0;
    if (ext == ".jpg" || ext == ".jpeg")
        ok = stbi_write_jpg_to_func(scan_write_cb, &buf,
                                    dst_w, dst_h, ch, write_px, candidate.quality);
    else if (ext == ".png")
        ok = stbi_write_png_to_func(scan_write_cb, &buf,
                                    dst_w, dst_h, ch, write_px, dst_w * ch);
    else if (ext == ".bmp")
        ok = stbi_write_bmp_to_func(scan_write_cb, &buf,
                                    dst_w, dst_h, ch, write_px);
#ifdef BATCHPRESS_HAS_WEBP
    else if (ext == ".webp") {
        // WebP requires RGBA (4 channels) — convert if needed
        std::vector<uint8_t> rgba;
        const uint8_t* rgba_ptr = write_px;
        int rgba_stride = dst_w * 4;

        if (ch != 4) {
            rgba.resize(dst_w * dst_h * 4);
            if (ch == 3) {
                for (int i = 0; i < dst_w * dst_h; ++i) {
                    rgba[i*4+0] = write_px[i*3+0];
                    rgba[i*4+1] = write_px[i*3+1];
                    rgba[i*4+2] = write_px[i*3+2];
                    rgba[i*4+3] = 255;
                }
            } else if (ch == 1) {
                for (int i = 0; i < dst_w * dst_h; ++i) {
                    rgba[i*4+0] = rgba[i*4+1] = rgba[i*4+2] = write_px[i];
                    rgba[i*4+3] = 255;
                }
            } else {
                return 0;  // unsupported channel count
            }
            rgba_ptr = rgba.data();
        }

        uint8_t* webp_out = nullptr;
        size_t webp_size = WebPEncodeRGBA(rgba_ptr, dst_w, dst_h, rgba_stride,
                                          static_cast<float>(candidate.quality), &webp_out);
        if (webp_size > 0 && webp_out) {
            buf.insert(buf.end(), webp_out, webp_out + webp_size);
            WebPFree(webp_out);
            ok = 1;
        }
    }
#endif
    else
        ok = stbi_write_png_to_func(scan_write_cb, &buf,
                                    dst_w, dst_h, ch, write_px, dst_w * ch);

    return (ok && !buf.empty()) ? static_cast<uint64_t>(buf.size()) : 0;
}

// ── Select evenly-spaced sample indices ───────────────────────────────────────

static std::vector<size_t> pick_samples(size_t total, uint32_t max_samples) {
    std::vector<size_t> idx;
    if (total == 0) return idx;

    uint32_t n = (max_samples == 0 || max_samples >= static_cast<uint32_t>(total))
        ? static_cast<uint32_t>(total)
        : max_samples;

    // Guard against division by zero when only 1 sample
    if (n <= 1) {
        idx.push_back(0);
        return idx;
    }

    idx.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
        idx.push_back(static_cast<size_t>(
            static_cast<double>(i) / (n - 1) * (total - 1) + 0.5));

    // Deduplicate
    std::sort(idx.begin(), idx.end());
    idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
    return idx;
}

// ── Scan a single directory ───────────────────────────────────────────────────

static DirScanResult scan_directory(
    const fs::path&                  dir,
    const std::vector<ImageMeta>&    all_images,
    const std::vector<ScanCandidate>& candidates,
    uint32_t                          samples_per_dir,
    std::atomic<uint32_t>&            global_done,
    uint32_t                          global_total,
    const std::function<void(const std::string&, uint32_t, uint32_t)>& on_progress)
{
    DirScanResult result;
    result.directory   = dir;
    result.image_count = static_cast<uint32_t>(all_images.size());

    for (const auto& m : all_images)
        result.total_bytes += m.file_bytes;

    if (all_images.empty()) return result;

    // Pick sample indices
    auto sample_idx = pick_samples(all_images.size(), samples_per_dir);

    // For each candidate, encode samples and extrapolate
    for (const auto& cand : candidates) {
        uint64_t sample_original_bytes  = 0;
        uint64_t sample_encoded_bytes   = 0;
        uint32_t valid_samples          = 0;

        for (size_t idx : sample_idx) {
            const ImageMeta& m = all_images[idx];
            uint64_t encoded = encode_sample(m, cand);

            if (encoded > 0) {
                sample_original_bytes += m.file_bytes;
                sample_encoded_bytes  += encoded;
                ++valid_samples;
            }

            // Notify progress
            uint32_t done = global_done.fetch_add(1, std::memory_order_relaxed) + 1;
            if (on_progress)
                on_progress(m.path.filename().string(), done, global_total);
        }

        if (valid_samples == 0 || sample_original_bytes == 0) continue;

        // Compression ratio measured from samples
        double ratio = static_cast<double>(sample_encoded_bytes)
                     / static_cast<double>(sample_original_bytes);

        // Extrapolate to entire directory
        uint64_t projected = static_cast<uint64_t>(
            static_cast<double>(result.total_bytes) * ratio);

        double savings_pct = 100.0 * (1.0 - ratio);

        DirScanResult::CandidateResult cr;
        cr.candidate       = cand;
        cr.projected_bytes = projected;
        cr.savings_pct     = savings_pct;
        cr.samples_used    = valid_samples;
        result.candidates.push_back(cr);
    }

    // Sort candidates by savings descending
    std::sort(result.candidates.begin(), result.candidates.end(),
        [](const DirScanResult::CandidateResult& a,
           const DirScanResult::CandidateResult& b) {
            return a.savings_pct > b.savings_pct;
        });

    // Pick best
    if (!result.candidates.empty()) {
        result.best_candidate      = result.candidates.front().candidate;
        result.best_projected_bytes= result.candidates.front().projected_bytes;
        result.best_savings_pct    = result.candidates.front().savings_pct;
    }

    return result;
}

// ── Collect images grouped by directory ──────────────────────────────────────

static std::map<fs::path, std::vector<ImageMeta>>
collect_by_dir(const fs::path& root, bool recursive)
{
    std::map<fs::path, std::vector<ImageMeta>> groups;

    auto add = [&](const fs::directory_entry& e) {
        if (e.is_regular_file() && scan_supported(e.path())) {
            fs::path dir = e.path().parent_path();
            groups[dir].push_back(read_meta(e.path()));
        }
    };

    if (recursive) {
        for (const auto& e : fs::recursive_directory_iterator(
                root, fs::directory_options::skip_permission_denied))
            add(e);
    } else {
        for (const auto& e : fs::directory_iterator(root))
            add(e);
    }

    return groups;
}

// ── run_scan ──────────────────────────────────────────────────────────────────

ScanReport run_scan(const ScanConfig& cfg) {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();

    ScanReport report;
    report.root_dir = cfg.root_dir;

    // Build candidate list
    auto candidates = cfg.candidates.empty()
        ? default_scan_candidates()
        : cfg.candidates;

    // Collect and group images by directory
    auto groups = collect_by_dir(cfg.root_dir, cfg.recursive);

    // Count total images across all groups for progress reporting
    uint32_t total_images = 0;
    for (const auto& [dir, images] : groups) {
        total_images += static_cast<uint32_t>(images.size());
        for (const auto& m : images) {
            report.total_images += 1;
            report.total_bytes  += m.file_bytes;
        }
    }

    // Total samples that will be encoded (for progress counter)
    uint32_t total_samples = 0;
    for (const auto& [dir, images] : groups) {
        uint32_t n = (cfg.samples_per_dir == 0 || cfg.samples_per_dir >= images.size())
            ? static_cast<uint32_t>(images.size())
            : cfg.samples_per_dir;
        total_samples += n * static_cast<uint32_t>(candidates.size());
    }

    std::atomic<uint32_t> global_done{0};

    // Scan each directory in parallel (one task per directory)
    size_t threads = cfg.num_threads > 0
        ? cfg.num_threads
        : std::thread::hardware_concurrency();

    ThreadPool pool(threads);

    using FutureDir = std::future<DirScanResult>;
    std::vector<FutureDir> futures;
    futures.reserve(groups.size());

    for (const auto& [dir, images] : groups) {
        futures.push_back(pool.submit(
            scan_directory,
            dir,
            images,
            std::cref(candidates),
            cfg.samples_per_dir,
            std::ref(global_done),
            total_samples,
            std::cref(cfg.on_progress)
        ));
    }

    // Collect results
    report.directories.reserve(groups.size());
    for (auto& fut : futures) {
        DirScanResult dr = fut.get();
        report.projected_bytes += dr.best_projected_bytes;
        report.directories.push_back(std::move(dr));
    }

    // Sort directories by bytes_saved descending (biggest win first)
    std::sort(report.directories.begin(), report.directories.end(),
        [](const DirScanResult& a, const DirScanResult& b) {
            return a.bytes_saved() > b.bytes_saved();
        });

    report.elapsed_sec = std::chrono::duration<double>(
                             Clock::now() - t0).count();
    return report;
}

// ── scan_files (per-file metadata with projected savings) ─────────────────────

static std::string format_to_string(ImageFormat fmt) {
    switch (fmt) {
        case ImageFormat::JPEG: return "JPEG";
        case ImageFormat::PNG:  return "PNG";
        case ImageFormat::BMP:  return "BMP";
        case ImageFormat::WebP: return "WebP";
        default:                return "Same";
    }
}

// Collect all image files (flat list, not grouped by dir)
static std::vector<fs::path> collect_all_images(const fs::path& root, bool recursive) {
    std::vector<fs::path> images;

    auto add = [&](const fs::directory_entry& e) {
        if (e.is_regular_file() && scan_supported(e.path())) {
            images.push_back(e.path());
        }
    };

    if (recursive) {
        for (const auto& e : fs::recursive_directory_iterator(
                root, fs::directory_options::skip_permission_denied))
            add(e);
    } else {
        for (const auto& e : fs::directory_iterator(root))
            add(e);
    }

    std::sort(images.begin(), images.end());
    return images;
}

FileScanReport scan_files(const ScanConfig& cfg) {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();

    FileScanReport report;

    // Build candidate list
    auto candidates = cfg.candidates.empty()
        ? default_scan_candidates()
        : cfg.candidates;

    // Collect all images
    auto images = collect_all_images(cfg.root_dir, cfg.recursive);
    if (images.empty()) {
        report.elapsed_sec = std::chrono::duration<double>(Clock::now() - t0).count();
        return report;
    }

    // Read metadata for all images
    struct ImageWithMeta {
        fs::path path;
        ImageMeta meta;
    };
    std::vector<ImageWithMeta> all_metas;
    all_metas.reserve(images.size());
    for (auto& img : images) {
        ImageMeta m = read_meta(img);
        all_metas.push_back({img, m});
    }

    // Count total samples for progress
    uint32_t samples_per_file = (cfg.samples_per_dir == 0) ? 1 : cfg.samples_per_dir;
    uint32_t total_samples = static_cast<uint32_t>(images.size()) *
                             samples_per_file *
                             static_cast<uint32_t>(candidates.size());
    std::atomic<uint32_t> global_done{0};

    // Process each file in parallel
    size_t threads = cfg.num_threads > 0
        ? cfg.num_threads
        : std::thread::hardware_concurrency();

    ThreadPool pool(threads);

    using FutureItem = std::future<FileItem>;
    std::vector<FutureItem> futures;
    futures.reserve(all_metas.size());

    for (const auto& item : all_metas) {
        // Capture by value — safe for thread pool submission
        futures.push_back(pool.submit([meta = item.meta, img_path = item.path,
                                        &candidates, samples_per_file,
                                        &global_done, total_samples, &cfg]() -> FileItem {
            FileItem fi;
            fi.type = FileItem::Type::Image;
            fi.path = img_path;
            fi.filename = img_path.filename().string();
            fi.file_size = meta.file_bytes;
            fi.width = meta.width;
            fi.height = meta.height;

            // Timestamps (creation_time and last_access are optional — not portable on Linux)
            try {
                fi.last_modified = fs::last_write_time(img_path);
                fi.creation_time = fi.last_modified;  // best-effort fallback
                fi.last_access = fi.last_modified;
            } catch (...) {
                // If we can't get timestamps, leave them empty
            }

            // Image-specific metadata via std::variant
            ImageFileInfo img_info;
            img_info.format = meta.extension.empty() ? "UNKNOWN" : meta.extension.substr(1);
            std::transform(img_info.format.begin(), img_info.format.end(),
                           img_info.format.begin(), ::toupper);

            // Test each candidate and find the best projection
            uint64_t best_projected = meta.file_bytes;
            double best_savings = 0.0;
            std::string best_codec;
            bool best_has_resize = false;
            int best_quality = 90;
            ImageFormat best_fmt = ImageFormat::Same;

            for (const auto& cand : candidates) {
                uint64_t encoded = encode_sample(meta, cand);
                if (encoded > 0 && meta.file_bytes > 0) {
                    double ratio = static_cast<double>(encoded) /
                                   static_cast<double>(meta.file_bytes);
                    uint64_t projected = static_cast<uint64_t>(
                        static_cast<double>(meta.file_bytes) * ratio);
                    double savings = 100.0 * (1.0 - ratio);

                    if (projected < best_projected) {
                        best_projected = projected;
                        best_savings = savings;
                        best_codec = cand.label();
                        best_has_resize = cand.resize.active();
                        best_quality = cand.quality;
                        best_fmt = cand.format;
                    }
                }

                // Progress notification
                uint32_t done = global_done.fetch_add(1, std::memory_order_relaxed) + 1;
                if (cfg.on_progress) {
                    cfg.on_progress(meta.path.filename().string(), done, total_samples);
                }
            }

            // If no candidate produced a valid projection, estimate with a default ratio
            if (best_projected == meta.file_bytes && meta.file_bytes > 0) {
                best_projected = static_cast<uint64_t>(
                    static_cast<double>(meta.file_bytes) * 0.6);
                best_savings = 40.0;
                best_quality = 85;
                best_fmt = ImageFormat::WebP;
            }

            // Estimate quality level based on compression config
            if (best_fmt == ImageFormat::PNG && !best_has_resize) {
                img_info.quality = QualityEstimate::Lossless;
            } else if (best_savings < 20.0) {
                img_info.quality = QualityEstimate::NearLossless;
            } else if (best_savings < 50.0 && !best_has_resize) {
                img_info.quality = QualityEstimate::High;
            } else if (best_savings < 75.0) {
                img_info.quality = QualityEstimate::Medium;
            } else {
                img_info.quality = QualityEstimate::Low;
            }

            fi.projected_size = best_projected;
            fi.savings_pct = best_savings;
            img_info.suggested_codec = best_codec;
            fi.meta = std::move(img_info);

            return fi;
        }));
    }

    // Collect results
    report.files.reserve(all_metas.size());
    for (auto& fut : futures) {
        report.files.push_back(fut.get());
    }

    // Sort by savings_pct descending (biggest savings first)
    std::sort(report.files.begin(), report.files.end(),
        [](const FileItem& a, const FileItem& b) {
            return a.savings_pct > b.savings_pct;
        });

    report.elapsed_sec = std::chrono::duration<double>(Clock::now() - t0).count();
    return report;
}

} // namespace batchpress
