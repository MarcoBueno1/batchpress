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

#pragma once

#include "export.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <optional>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <variant>

namespace batchpress {

namespace fs = std::filesystem;

// ── Hash Cache for duplicate detection ───────────────────────────────────────

/**
 * @brief Thread-safe cache for file hashes and their processed output paths.
 *
 * Not copyable or movable — always used via shared_ptr to prevent
 * accidental data races during concurrent access.
 */
class BATCHPRESS_API HashCache {
public:
    HashCache() = default;
    HashCache(const HashCache&) = delete;
    HashCache& operator=(const HashCache&) = delete;
    HashCache(HashCache&&) = delete;
    HashCache& operator=(HashCache&&) = delete;

    /// Returns the cached output path, or nullopt if not found.
    std::optional<fs::path> get(const std::string& input_sha256);
    void put(const std::string& input_sha256, const fs::path& output_path);
    void clear();

private:
    std::unordered_map<std::string, fs::path> cache_;
    mutable std::shared_mutex mutex_;
};

// ── Output format ─────────────────────────────────────────────────────────────

enum class BATCHPRESS_API ImageFormat {
    Same,   ///< Keep original format
    JPEG,
    PNG,
    BMP,
    WebP,
};

BATCHPRESS_API ImageFormat parse_format(const std::string& s);
BATCHPRESS_API std::string format_extension(ImageFormat fmt);

// ── Resize mode ───────────────────────────────────────────────────────────────

struct BATCHPRESS_API ResizeSpec {
    enum class Mode { None, Exact, Fit, Percent } mode = Mode::None;
    uint32_t width  = 0;
    uint32_t height = 0;
    float    pct    = 1.0f;

    bool active() const noexcept { return mode != Mode::None; }
    std::pair<uint32_t, uint32_t> compute(uint32_t w, uint32_t h) const noexcept;
};

BATCHPRESS_API ResizeSpec parse_resize(const std::string& s);

// ── Write strategy ────────────────────────────────────────────────────────────

/**
 * @brief Describes how a file was written to disk.
 *
 * Safe   — encode to temp file → atomic rename(). Crash-safe.
 *          Requires free disk space >= encoded size + margin.
 *
 * Direct — overwrite original file in-place (O_TRUNC + ftruncate).
 *          Uses zero extra disk space. Not crash-safe.
 *          SHA-256 of original is recorded in TaskResult before writing.
 */
enum class BATCHPRESS_API WriteMode { Safe, Direct };

// ── Per-task result ───────────────────────────────────────────────────────────

struct BATCHPRESS_API TaskResult {
    fs::path    input_path;
    fs::path    output_path;
    bool        success         = false;
    bool        skipped         = false;
    bool        is_duplicate    = false;  ///< True if skipped due to duplicate detection (not skip_existing)
    bool        dry_run         = false;
    WriteMode   write_mode      = WriteMode::Safe;
    uint64_t    input_bytes     = 0;
    uint64_t    output_bytes    = 0;    ///< Actual or estimated (dry-run)
    double      elapsed_ms      = 0.0;
    std::string original_sha256;        ///< Set when WriteMode::Direct is used
    std::string error_msg;
};

// ── Progress callback ─────────────────────────────────────────────────────────

/**
 * @brief Called from a worker thread after each task completes.
 *
 * The UI layer provides this callback. The core never touches stdout/stderr.
 * Implementations must be thread-safe (use mutex or post to UI thread).
 *
 * @param result   Outcome of the processed image
 * @param done     Number of tasks finished so far
 * @param total    Total number of tasks in the batch
 */
using ProgressCallback = std::function<void(
    const TaskResult& result,
    uint64_t done,
    uint64_t total
)>;

// ── Config ────────────────────────────────────────────────────────────────────

struct BATCHPRESS_API Config {
    fs::path    input_dir;
    fs::path    output_dir;         ///< Empty = in-place (default)
    ResizeSpec  resize;
    ImageFormat format        = ImageFormat::Same;
    int         quality       = 90;
    bool        recursive     = true;
    bool        skip_existing = true;
    bool        dry_run       = false;
    bool        dedup_enabled = true;   ///< Enable duplicate detection via hash cache (default: on)
    size_t      num_threads   = 0;  ///< 0 = hardware_concurrency

    /**
     * @brief Called after every processed image. Thread-safe.
     *
     * Assign this before calling run_batch(). The core calls it from
     * worker threads — the UI is responsible for thread-safe dispatch
     * (e.g. Qt::QueuedConnection, Android runOnUiThread, mutex + cout).
     */
    ProgressCallback on_progress;

    /// True when output_dir is empty (in-place mode)
    bool inplace() const noexcept { return output_dir.empty(); }

    /// Hash cache for duplicate detection (shared across all workers)
    std::shared_ptr<HashCache> hash_cache;
};

// ── File Item for selective processing ────────────────────────────────────────

/**
 * @brief Estimated quality level after compression.
 *
 * Lossless      — pixel-perfect (PNG, no resize)
 * NearLossless  — visually identical, tiny differences (WebP q95+)
 * High          — excellent for archival, minor differences (WebP q85-90)
 * Medium        — good for everyday use, visible on close inspection (WebP q60-75)
 * Low           — noticeable artifacts, maximum compression (WebP q40-50)
 */
enum class BATCHPRESS_API QualityEstimate {
    Lossless,       ///< Pixel-perfect (PNG, no resize)
    NearLossless,   ///< Visually identical (<0.1% pixel difference)
    High,           ///< Excellent quality, minor differences
    Medium,         ///< Good for everyday use
    Low             ///< Noticeable artifacts, maximum compression
};

/// Human-readable label for quality level
BATCHPRESS_API const char* quality_label(QualityEstimate q);

/// Star rating for quality (1-5 stars)
BATCHPRESS_API int quality_stars(QualityEstimate q);

/**
 * @brief Image-specific metadata for FileItem.
 */
struct BATCHPRESS_API ImageFileInfo {
    std::string format;              ///< e.g. "JPEG", "PNG", "WebP"
    std::string suggested_codec;     ///< e.g. "WebP q85 fit:1920x1080"
    QualityEstimate quality = QualityEstimate::High;  ///< Estimated quality after compression
    uint32_t projected_width  = 0;   ///< Output width after suggested resize (0 = same as input)
    uint32_t projected_height = 0;   ///< Output height after suggested resize (0 = same as input)
};

/**
 * @brief Video-specific metadata for FileItem.
 */
struct BATCHPRESS_API VideoFileInfo {
    double      duration_sec = 0.0;  ///< Video duration in seconds
    std::string video_codec;         ///< Current video codec name (e.g. "h264")
    std::string audio_codec;         ///< Current audio codec name (e.g. "aac")
    std::string container;           ///< Container format (e.g. "mp4")
    std::string suggested_codec;     ///< e.g. "H.265 CRF28"
    QualityEstimate quality = QualityEstimate::High;  ///< Estimated quality after compression
    uint32_t projected_width  = 0;   ///< Output width after resolution cap (0 = same)
    uint32_t projected_height = 0;   ///< Output height after resolution cap (0 = same)
};

/**
 * @brief Represents a single media file with metadata and projected savings.
 *
 * Returned by scan_files() so UI can display per-file details and let
 * the user choose which files to actually process.
 *
 * Uses std::variant<ImageFileInfo, VideoFileInfo> to enforce mutual
 * exclusivity — an image item cannot accidentally hold video metadata.
 */
struct BATCHPRESS_API FileItem {
    enum class Type { Image, Video } type = Type::Image;

    fs::path    path;                   ///< Absolute path to the file
    std::string filename;               ///< Just the filename with extension

    // Timestamps (creation_time is optional — not portable on Linux)
    std::optional<std::filesystem::file_time_type> creation_time;
    std::optional<std::filesystem::file_time_type> last_access;
    std::filesystem::file_time_type last_modified{};  ///< Always available

    // Dimensions (images: pixels, videos: resolution)
    uint32_t    width  = 0;
    uint32_t    height = 0;

    // File size
    uint64_t    file_size = 0;          ///< File size in bytes

    // Projected compression results (estimated by scan)
    uint64_t    projected_size = 0;     ///< Estimated size after compression
    double      savings_pct = 0.0;      ///< Expected savings percentage (0-100)

    // Type-specific metadata — enforced by std::variant
    std::variant<ImageFileInfo, VideoFileInfo> meta;

    // Computed helpers
    uint64_t projected_savings() const noexcept {
        return (file_size > projected_size) ? (file_size - projected_size) : 0;
    }

    // Convenience accessors
    const ImageFileInfo& image_info() const { return std::get<ImageFileInfo>(meta); }
    const VideoFileInfo& video_info() const { return std::get<VideoFileInfo>(meta); }
};

/**
 * @brief Report returned by scan_files().
 *
 * Contains a flat list of all discovered files with per-file metadata
 * and projected savings estimates.
 */
struct BATCHPRESS_API FileScanReport {
    std::vector<FileItem> files;
    double   elapsed_sec = 0.0;

    uint64_t total_size() const noexcept {
        uint64_t sum = 0;
        for (auto& f : files) sum += f.file_size;
        return sum;
    }
    uint64_t total_projected_size() const noexcept {
        uint64_t sum = 0;
        for (auto& f : files) sum += f.projected_size;
        return sum;
    }
    uint64_t total_savings() const noexcept {
        uint64_t total = total_size();
        uint64_t projected = total_projected_size();
        return (total > projected) ? (total - projected) : 0;
    }
    double overall_savings_pct() const noexcept {
        uint64_t total = total_size();
        if (total == 0) return 0.0;
        return 100.0 * (1.0 - static_cast<double>(total_projected_size()) / static_cast<double>(total));
    }
    uint32_t image_count() const noexcept {
        uint32_t c = 0;
        for (auto& f : files) if (f.type == FileItem::Type::Image) c++;
        return c;
    }
    uint32_t video_count() const noexcept {
        uint32_t c = 0;
        for (auto& f : files) if (f.type == FileItem::Type::Video) c++;
        return c;
    }
};

// ── Batch report ──────────────────────────────────────────────────────────────

struct BATCHPRESS_API BatchReport {
    uint32_t total               = 0;
    uint32_t succeeded           = 0;
    uint32_t skipped             = 0;
    uint32_t failed              = 0;
    uint32_t duplicates_found    = 0;  ///< Files skipped due to duplicate detection
    uint32_t written_safe        = 0;
    uint32_t written_direct      = 0;
    uint64_t input_bytes_total   = 0;
    uint64_t output_bytes_total  = 0;
    double   elapsed_sec         = 0.0;
    bool     dry_run             = false;

    double throughput() const noexcept {
        return elapsed_sec > 0 ? static_cast<double>(succeeded) / elapsed_sec : 0.0;
    }
    int64_t bytes_saved() const noexcept {
        return static_cast<int64_t>(input_bytes_total) - static_cast<int64_t>(output_bytes_total);
    }
    double savings_pct() const noexcept {
        if (input_bytes_total == 0) return 0.0;
        return 100.0 * (1.0 - static_cast<double>(output_bytes_total) / static_cast<double>(input_bytes_total));
    }
};

// ── Disk utility ──────────────────────────────────────────────────────────────

/// Returns available bytes on the filesystem containing @p path.
BATCHPRESS_API uint64_t disk_free_bytes(const fs::path& path) noexcept;

} // namespace batchpress
