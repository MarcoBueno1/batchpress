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

#include "types.hpp"
#include <vector>
#include <string>
#include <cstdint>

namespace batchpress {

// ── Per-image metadata (read without decoding pixels) ─────────────────────────

struct BATCHPRESS_API ImageMeta {
    fs::path path;
    uint32_t width        = 0;
    uint32_t height       = 0;
    int      channels     = 0;
    uint64_t file_bytes   = 0;
    std::string extension;          ///< lowercase, e.g. ".jpg"
};

// ── Candidate configuration tested during scan ────────────────────────────────

struct BATCHPRESS_API ScanCandidate {
    ImageFormat format  = ImageFormat::Same;
    int         quality = 85;
    ResizeSpec  resize;

    /// Human-readable label, e.g. "WebP 85% fit:1920x1080"
    std::string label() const;
};

// ── Result for a single subdirectory ─────────────────────────────────────────

struct BATCHPRESS_API DirScanResult {
    fs::path    directory;
    uint32_t    image_count       = 0;
    uint64_t    total_bytes       = 0;      ///< Current size on disk

    /// Best suggestion found across all candidates
    ScanCandidate   best_candidate;
    uint64_t        best_projected_bytes = 0;   ///< Estimated size after applying best
    double          best_savings_pct     = 0.0;

    /// All candidates tested, sorted by savings descending
    struct CandidateResult {
        ScanCandidate candidate;
        uint64_t      projected_bytes = 0;
        double        savings_pct     = 0.0;
        uint32_t      samples_used    = 0;  ///< How many images were actually encoded
    };
    std::vector<CandidateResult> candidates;

    int64_t bytes_saved() const noexcept {
        return static_cast<int64_t>(total_bytes)
             - static_cast<int64_t>(best_projected_bytes);
    }

    /// Star rating 1–3 based on savings percentage
    int stars() const noexcept {
        if (best_savings_pct >= 80.0) return 3;
        if (best_savings_pct >= 50.0) return 2;
        return 1;
    }
};

// ── Full scan report ──────────────────────────────────────────────────────────

struct BATCHPRESS_API ScanReport {
    fs::path root_dir;
    uint32_t total_images     = 0;
    uint64_t total_bytes      = 0;
    uint64_t projected_bytes  = 0;  ///< If best suggestion applied everywhere
    double   elapsed_sec      = 0.0;

    std::vector<DirScanResult> directories;  ///< One entry per subdirectory

    int64_t bytes_saved()   const noexcept;
    double  savings_pct()   const noexcept;

    /// Returns the single best ScanCandidate across ALL directories
    /// (the one that would save the most total space)
    ScanCandidate global_best_candidate() const;

    /// Command-line string to apply the global best suggestion
    std::string suggested_command(const std::string& exe_name = "batchpress") const;
};

// ── Scan configuration ────────────────────────────────────────────────────────

struct BATCHPRESS_API ScanConfig {
    fs::path    root_dir;
    bool        recursive        = true;

    /**
     * @brief Number of sample images to encode per directory per candidate.
     *
     * The scanner picks evenly-spaced images from the directory,
     * encodes them in RAM, measures the compression ratio, then
     * extrapolates the ratio to the remaining images.
     *
     * Higher = more accurate, slower.
     * 0 = encode ALL images (same as dry-run, most accurate).
     */
    uint32_t    samples_per_dir  = 5;

    /// Candidate configurations to test. If empty, defaults are used.
    std::vector<ScanCandidate> candidates;

    /// Progress callback: called after each image is sampled.
    /// (filename, images_done, images_total)
    std::function<void(const std::string&, uint32_t, uint32_t)> on_progress;

    size_t      num_threads      = 0;   ///< 0 = hardware_concurrency
};

// ── Default candidates tested when ScanConfig::candidates is empty ────────────

BATCHPRESS_API std::vector<ScanCandidate> default_scan_candidates();

// ── Public API ────────────────────────────────────────────────────────────────

/**
 * @brief Scans a directory tree and estimates space savings per subdirectory.
 *
 * For each subdirectory:
 *   1. Reads image metadata (width, height, file size) without decoding
 *   2. Picks up to ScanConfig::samples_per_dir representative images
 *   3. Encodes each sample in RAM for every candidate configuration
 *   4. Extrapolates compression ratio to the full directory
 *   5. Selects the best candidate (highest savings)
 *
 * Never writes to disk. Never modifies files.
 * All UI feedback goes through ScanConfig::on_progress.
 */
BATCHPRESS_API ScanReport run_scan(const ScanConfig& cfg);

// ── File-level scan API (for selective processing) ────────────────────────────

/**
 * @brief Scans a directory tree and returns per-file metadata with projected savings.
 *
 * Unlike run_scan() which aggregates by directory, this function returns
 * a flat list of FileItem — one per file — so the UI can let the user
 * pick which files to process.
 *
 * For each file it collects:
 *   - Name, path, timestamps, type, dimensions, file size
 *   - Projected size after compression and savings percentage
 *   - Suggested codec / format configuration
 *
 * Never writes to disk. Never modifies files.
 *
 * @param cfg        Scan configuration (root_dir, recursive, candidates, threads)
 * @return           FileScanReport with one FileItem per discovered file
 */
BATCHPRESS_API FileScanReport scan_files(const ScanConfig& cfg);

} // namespace batchpress
