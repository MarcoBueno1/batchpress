// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.marco@gmail.com>
 *
 * This file is part of batchpress — Interactive file selection UI.
 */

#pragma once

#include <batchpress/types.hpp>
#include <batchpress/scanner.hpp>
#include <batchpress/video_processor.hpp>
#include <vector>
#include <string>

namespace cli {

/// Result of interactive file selection
struct SelectResult {
    std::vector<batchpress::FileItem> selected;  ///< Files chosen by user
    bool proceed_with_processing = false;         ///< true if user pressed Enter to process
};

/// Run interactive file selection UI
/// Returns selected files and whether to proceed with processing
SelectResult run_select_ui(
    const batchpress::FileScanReport& report,
    const batchpress::Config& img_cfg,
    const batchpress::VideoConfig& vid_cfg,
    const std::string& filter_type,    // "image", "video", or "all"
    double min_savings_pct              // only show files >= this savings
);

} // namespace cli
