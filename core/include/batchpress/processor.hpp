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

namespace batchpress {

/**
 * @brief Scans input_dir for supported image files.
 *
 * Respects cfg.recursive. Returns paths sorted for determinism.
 * Supported: .jpg .jpeg .png .bmp .tga .hdr .pic .pnm
 */
BATCHPRESS_API std::vector<fs::path> collect_images(const Config& cfg);

/**
 * @brief Processes a single image file according to Config.
 *
 * - Loads the file into RAM (stb_image)
 * - Resizes in RAM if requested (stb_image_resize)
 * - Encodes to target format in RAM (stb_image_write to memory)
 * - Writes to disk using the adaptive strategy:
 *     · WriteMode::Safe   — temp file + atomic rename (crash-safe)
 *     · WriteMode::Direct — overwrite in-place (zero extra disk space)
 *
 * Never writes to stdout/stderr. All feedback goes through Config::on_progress.
 *
 * Thread-safe: can be called concurrently from multiple threads.
 */
BATCHPRESS_API TaskResult process_image(const fs::path& input_path,
                                       const Config&   cfg);

/**
 * @brief Runs the full batch pipeline.
 *
 * 1. Collects images via collect_images()
 * 2. Submits each to the internal ThreadPool
 * 3. Calls cfg.on_progress() after each task (from worker threads)
 * 4. Returns aggregated BatchReport when all tasks finish
 *
 * Blocking call — returns only after all images are processed.
 */
BATCHPRESS_API BatchReport run_batch(const Config& cfg);

// ── Selective file processing API ─────────────────────────────────────────────

/**
 * @brief Processes a user-selected list of image files.
 *
 * Instead of scanning and processing everything automatically,
 * this function accepts a pre-filtered list of FileItem (images only)
 * and processes each one using the provided Config.
 *
 * The FileItem list should come from a prior call to scan_files().
 * The UI can filter / sort / let the user pick items before calling this.
 *
 * @param files      List of image FileItems to process (from scan_files)
 * @param cfg        Processing configuration (resize, format, quality, etc.)
 * @return           Aggregated BatchReport for the processed files
 */
BATCHPRESS_API BatchReport process_files(const std::vector<FileItem>& files,
                                         const Config& cfg);

} // namespace batchpress
