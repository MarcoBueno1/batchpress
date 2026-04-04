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
#include "types.hpp"  // Para HashCache
#include <filesystem>
#include <functional>
#include <string>
#include <cstdint>
#include <vector>

namespace batchpress {

namespace fs = std::filesystem;

// ── Video codec selected at runtime ──────────────────────────────────────────

/**
 * @brief Video codec priority order.
 *
 * The core probes libavcodec at runtime and selects the best available:
 *   1. H.265 (HEVC)  — best compression, ~50% smaller than H.264
 *   2. H.264 (AVC)   — universal compatibility
 *   3. VP9            — open source, royalty-free, always in ffmpeg-kit
 *
 * AudioCodec is selected to match the container and maximise compression:
 *   H.265/H.264 → AAC
 *   VP9         → Opus
 */
enum class BATCHPRESS_API VideoCodec { Auto, H265, H264, VP9 };
enum class BATCHPRESS_API AudioCodec { Auto, AAC, Opus, None };

/// Container format inferred from output codec when not specified.
enum class BATCHPRESS_API VideoContainer { Auto, MP4, WebM, MKV };

// ── Runtime codec capabilities ────────────────────────────────────────────────

struct BATCHPRESS_API CodecCaps {
    bool has_h265   = false;
    bool has_h264   = false;
    bool has_vp9    = false;
    bool has_aac    = false;
    bool has_opus   = false;

    /// Returns the best available video codec.
    VideoCodec best_video() const noexcept;

    /// Returns the best audio codec for a given video codec.
    AudioCodec best_audio(VideoCodec vc) const noexcept;
};

/// Probes libavcodec and returns available codec capabilities.
BATCHPRESS_API CodecCaps probe_codec_caps() noexcept;

// ── Audio content classification ─────────────────────────────────────────────

/**
 * @brief Detected audio content type — used to select target bitrate.
 *
 * Speech  → low bitrate sufficient (48–64 kbps)
 * Music   → higher bitrate needed  (96–128 kbps)
 * Silent  → audio stream removed entirely
 * Unknown → treat as Music (safe default)
 */
enum class BATCHPRESS_API AudioContent { Speech, Music, Silent, Unknown };

// ── Resolution cap ────────────────────────────────────────────────────────────

/**
 * @brief Maximum output resolution policy.
 *
 * Original  — keep source resolution
 * Cap1080p  — downscale if > 1080p  (default for MaxCompression)
 * Cap4K     — downscale if > 4K
 */
enum class BATCHPRESS_API ResolutionCap { Original, Cap1080p, Cap4K };

// ── Video processing config ───────────────────────────────────────────────────

struct BATCHPRESS_API VideoConfig {
    fs::path       input_dir;
    fs::path       output_dir;      ///< empty = in-place
    bool           recursive    = true;
    bool           dry_run      = false;
    bool           skip_existing= true;
    bool           dedup_enabled= true;   ///< Enable duplicate detection via hash cache
    size_t         num_threads  = 0; ///< 0 = hardware_concurrency

    // ── Codec selection ───────────────────────────────────────────────────
    VideoCodec     video_codec  = VideoCodec::Auto;  ///< Auto = probe at runtime
    AudioCodec     audio_codec  = AudioCodec::Auto;
    VideoContainer container    = VideoContainer::Auto;

    // ── Quality ───────────────────────────────────────────────────────────
    /**
     * CRF value. -1 = automatic (recommended):
     *   H.265 → CRF 28
     *   H.264 → CRF 26
     *   VP9   → CRF 33
     * Lower = better quality, larger file.
     */
    int            crf          = -1;

    // ── Resolution ────────────────────────────────────────────────────────
    ResolutionCap  resolution   = ResolutionCap::Cap1080p;

    // ── Audio ─────────────────────────────────────────────────────────────
    /// -1 = auto (48kbps for speech, 96kbps for music, removed if silent)
    int            audio_bitrate_kbps = -1;

    // ── Progress callback ─────────────────────────────────────────────────
    /**
     * Called from worker threads during processing.
     * @param path        Current file being processed
     * @param frame_done  Frames encoded so far for this file
     * @param frame_total Total frames in this file (0 if unknown)
     * @param files_done  Files completed so far
     * @param files_total Total files in batch
     */
    std::function<void(
        const fs::path& path,
        uint64_t frame_done,
        uint64_t frame_total,
        uint32_t files_done,
        uint32_t files_total
    )> on_progress;

    bool inplace() const noexcept { return output_dir.empty(); }

    /// Hash cache for duplicate detection (shared across all workers)
    std::shared_ptr<HashCache> hash_cache;
};

// ── Per-file video metadata ───────────────────────────────────────────────────

struct BATCHPRESS_API VideoMeta {
    fs::path      path;
    uint64_t      file_bytes    = 0;
    double        duration_sec  = 0.0;
    int           width         = 0;
    int           height        = 0;
    double        fps           = 0.0;
    uint64_t      frame_count   = 0;
    int64_t       video_bitrate = 0;   ///< bits/s
    int64_t       audio_bitrate = 0;   ///< bits/s
    std::string   video_codec_name;
    std::string   audio_codec_name;
    std::string   container_name;
    AudioContent  audio_content = AudioContent::Unknown;
    bool          readable      = false;
};

// ── Per-file result ───────────────────────────────────────────────────────────

struct BATCHPRESS_API VideoResult {
    fs::path      input_path;
    fs::path      output_path;
    bool          success        = false;
    bool          skipped        = false;
    bool          is_duplicate   = false;  ///< True if skipped due to duplicate detection
    bool          dry_run        = false;
    uint64_t      input_bytes    = 0;
    uint64_t      output_bytes   = 0;
    double        elapsed_sec    = 0.0;
    VideoCodec    codec_used     = VideoCodec::Auto;
    AudioCodec    audio_used     = AudioCodec::Auto;
    int           crf_used       = 0;
    AudioContent  audio_content  = AudioContent::Unknown;
    std::string   error_msg;

    double compression_ratio() const noexcept {
        if (input_bytes == 0) return 0.0;
        return 1.0 - static_cast<double>(output_bytes)
                   / static_cast<double>(input_bytes);
    }
};

// ── Batch report ──────────────────────────────────────────────────────────────

struct BATCHPRESS_API VideoBatchReport {
    uint32_t total               = 0;
    uint32_t succeeded           = 0;
    uint32_t skipped             = 0;
    uint32_t failed              = 0;
    uint32_t duplicates_found    = 0;  ///< Files skipped due to duplicate detection
    uint64_t input_bytes_total   = 0;
    uint64_t output_bytes_total  = 0;
    double   elapsed_sec         = 0.0;
    bool     dry_run             = false;

    // Codec usage breakdown
    uint32_t used_h265 = 0;
    uint32_t used_h264 = 0;
    uint32_t used_vp9  = 0;

    double throughput()  const noexcept;
    int64_t bytes_saved() const noexcept;
    double savings_pct() const noexcept;
};

// ── Scan types ────────────────────────────────────────────────────────────────

struct BATCHPRESS_API VideoScanConfig {
    fs::path    root_dir;
    bool        recursive     = true;
    size_t      num_threads   = 0;
    std::function<void(const std::string&, uint32_t, uint32_t)> on_progress;
};

struct BATCHPRESS_API VideoDirScanResult {
    fs::path    directory;
    uint32_t    video_count       = 0;
    uint64_t    total_bytes       = 0;
    uint64_t    total_duration_sec= 0;

    // Best projected outcome using auto codec selection
    VideoCodec  suggested_codec   = VideoCodec::Auto;
    int         suggested_crf     = 0;
    ResolutionCap suggested_res   = ResolutionCap::Cap1080p;
    uint64_t    projected_bytes   = 0;
    double      savings_pct       = 0.0;

    int64_t bytes_saved() const noexcept {
        return static_cast<int64_t>(total_bytes)
             - static_cast<int64_t>(projected_bytes);
    }

    int stars() const noexcept {
        if (savings_pct >= 60.0) return 3;
        if (savings_pct >= 30.0) return 2;
        return 1;
    }

    /// CLI command to apply the suggestion
    std::string suggested_command(const std::string& exe = "batchpress") const;
};

struct BATCHPRESS_API VideoScanReport {
    fs::path    root_dir;
    uint32_t    total_videos      = 0;
    uint64_t    total_bytes       = 0;
    uint64_t    projected_bytes   = 0;
    double      elapsed_sec       = 0.0;
    CodecCaps   caps;

    std::vector<VideoDirScanResult> directories;

    int64_t bytes_saved()  const noexcept;
    double  savings_pct()  const noexcept;
    std::string suggested_command(const std::string& exe = "batchpress") const;
};

// ── Public API ────────────────────────────────────────────────────────────────

/// Read video metadata without decoding frames.
BATCHPRESS_API VideoMeta read_video_meta(const fs::path& path);

/**
 * @brief Transcodes a single video file with adaptive codec selection.
 *
 * - Probes libavcodec at runtime: H.265 > H.264 > VP9
 * - Classifies audio content (speech/music/silent) → selects bitrate
 * - Applies resolution cap (default: 1080p)
 * - Uses maximum CRF for highest compression with acceptable quality
 * - In-place: writes to .tmp then renames atomically
 *
 * Thread-safe. Never writes to stdout/stderr.
 */
BATCHPRESS_API VideoResult process_video(const fs::path& input_path,
                                        const VideoConfig& cfg);

/**
 * @brief Runs a full video batch processing pipeline.
 *
 * Uses half of available CPU cores by default (video encoding is CPU-heavy).
 * Calls cfg.on_progress from worker threads.
 */
BATCHPRESS_API VideoBatchReport run_video_batch(const VideoConfig& cfg);

/**
 * @brief Scans a directory tree and estimates video compression savings.
 *
 * Reads metadata only (no decoding). Projects savings using empirical
 * CRF compression ratios for the best available codec on this system.
 */
BATCHPRESS_API VideoScanReport run_video_scan(const VideoScanConfig& cfg);

// ── Selective video file processing API ───────────────────────────────────────

/**
 * @brief Processes a user-selected list of video files.
 *
 * Instead of scanning and processing everything automatically,
 * this function accepts a pre-filtered list of FileItem (videos only)
 * and processes each one using the provided VideoConfig.
 *
 * The FileItem list should come from a prior call to scan_files().
 * The UI can filter / sort / let the user pick items before calling this.
 *
 * @param files      List of video FileItems to process (from scan_files)
 * @param cfg        Processing configuration (codec, CRF, resolution, etc.)
 * @return           Aggregated VideoBatchReport for the processed files
 */
BATCHPRESS_API VideoBatchReport process_video_files(const std::vector<FileItem>& files,
                                                     const VideoConfig& cfg);

} // namespace batchpress
