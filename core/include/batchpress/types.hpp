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
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <shared_mutex>

namespace batchpress {

namespace fs = std::filesystem;

// ── Hash Cache for duplicate detection ───────────────────────────────────────

/**
 * @brief Thread-safe cache for file hashes and their processed output paths.
 */
class BATCHPRESS_API HashCache {
public:
    const fs::path* get(const std::string& input_sha256);
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
    uint32_t done,
    uint32_t total
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

    double  throughput()  const noexcept;
    int64_t bytes_saved() const noexcept;
    double  savings_pct() const noexcept;
};

// ── Disk utility ──────────────────────────────────────────────────────────────

/// Returns available bytes on the filesystem containing @p path.
BATCHPRESS_API uint64_t disk_free_bytes(const fs::path& path) noexcept;

} // namespace batchpress
