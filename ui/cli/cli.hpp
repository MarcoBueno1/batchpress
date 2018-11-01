// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.marco@gmail.com>
 *
 * This file is part of batchpress — CLI argument parser.
 */

#pragma once

#include <batchpress/types.hpp>
#include <batchpress/scanner.hpp>
#include <batchpress/video_processor.hpp>
#include <string>
#include <stdexcept>

namespace cli {

/// Operating mode selected by the user.
enum class Mode {
    Process,        ///< Batch process images + videos (default, in-place or copy)
    DryRun,         ///< Encode in RAM only — report projected savings
    Scan,           ///< Analyse images — suggest best config
    ScanVideo,      ///< Analyse videos — suggest best codec/CRF
    ScanAll,        ///< Analyse both images and videos
};

/// All parsed arguments.
struct Args {
    Mode                  mode       = Mode::Process;
    bool                  verbose    = false;
    batchpress::Config      process_cfg;       ///< Process / DryRun (images)
    batchpress::VideoConfig video_cfg;         ///< Process / DryRun (videos)
    batchpress::ScanConfig  scan_cfg;          ///< Image scan
    batchpress::VideoScanConfig vscan_cfg;     ///< Video scan
};

Args parse_args(int argc, char* argv[]);
void print_help(const char* program_name);

} // namespace cli
