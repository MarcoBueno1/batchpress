// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.mario@gmail.com>
 */

#pragma once
#include <batchpress/video_processor.hpp>

namespace cli {
void print_video_scan_report(const batchpress::VideoScanReport& r,
                              bool verbose = false);
} // namespace cli
