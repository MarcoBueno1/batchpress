// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.marco@gmail.com>
 *
 * This file is part of batchpress — CLI scan reporter.
 */

#pragma once

#include <batchpress/scanner.hpp>

namespace cli {

/**
 * @brief Prints a full scan report to stdout with ANSI colours.
 *
 * @param r        The ScanReport produced by batchpress::run_scan()
 * @param verbose  If true, show all tested candidates per directory
 */
void print_scan_report(const batchpress::ScanReport& r, bool verbose = false);

} // namespace cli
