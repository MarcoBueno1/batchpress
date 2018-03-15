// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.mario@gmail.com>
 *
 * This file is part of batchpress — CLI argument parser.
 */

#include "cli.hpp"
#include <iostream>
#include <thread>

namespace cli {

void print_help(const char* prog) {
    std::cout <<
R"(batchpress — Parallel image & video processor  |  C++17 + libav

USAGE:
  )" << prog << R"( --input <dir> [options]

REQUIRED:
  --input <dir>         Source directory (images + videos processed together)

MODES:
  (default)             Process images and videos in-place (adaptive safe/direct).
  --dry-run             Encode in RAM only. Print projected savings. Nothing written.
  --scan                Analyse images → suggest best format/resize per directory.
  --scan-video          Analyse videos → detect best codec/CRF for this system.
  --scan-all            Analyse both images and videos together.

OUTPUT:
  --output <dir>        Write to separate directory (disables in-place).

VIDEO OPTIONS:
  --vcodec <codec>      h265 | h264 | vp9 | auto  (default: auto = best available)
  --crf <n>             Video quality: lower = better quality, larger file.
                        Defaults: h265=28, h264=26, vp9=33
  --max-res <res>       1080p | 4k | original      (default: 1080p)

IMAGE OPTIONS:
  --resize  <spec>      1920x1080 | 50% | fit:1280x720
  --format  <fmt>       jpg | png | bmp | webp | same  (default: same)
  --quality <1-100>     JPEG/WebP quality              (default: 90)

COMMON OPTIONS:
  --threads <n>         Worker threads (default: CPU cores)
  --no-recursive        Do not traverse subdirectories
  --overwrite           Overwrite existing output files (copy mode only)
  --samples <n>         Scan: images sampled per dir (default: 5, 0=all)
  --verbose             Per-file details or all scan candidates
  --help                Show this help

EXAMPLES:
  # Scan everything first — see what you can save
  batchpress --input /sdcard/DCIM --scan-all

  # Apply in-place with best auto settings
  batchpress --input /sdcard/DCIM

  # Dry-run with explicit settings
  batchpress --input ./media/ --format webp --quality 85 --vcodec h265 --dry-run

  # Copy to separate directory, cap at 1080p
  batchpress --input ./raw/ --output ./compressed/ --max-res 1080p
)";
}

Args parse_args(int argc, char* argv[]) {
    if (argc < 2) { print_help(argv[0]); std::exit(0); }

    Args args;
    size_t hw = std::thread::hardware_concurrency();
    args.process_cfg.num_threads  = hw;
    args.video_cfg.num_threads    = hw;
    args.scan_cfg.num_threads     = hw;
    args.vscan_cfg.num_threads    = hw;
    args.scan_cfg.samples_per_dir = 5;

    auto next = [&](int i, const char* flag) -> const char* {
        if (i + 1 >= argc)
            throw std::invalid_argument(std::string(flag) + " requires a value");
        return argv[i + 1];
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") { print_help(argv[0]); std::exit(0); }

        // ── Modes ─────────────────────────────────────────────────────────
        else if (arg == "--dry-run")    { args.mode = Mode::DryRun;    }
        else if (arg == "--scan")       { args.mode = Mode::Scan;      }
        else if (arg == "--scan-video") { args.mode = Mode::ScanVideo; }
        else if (arg == "--scan-all")   { args.mode = Mode::ScanAll;   }

        // ── Input ─────────────────────────────────────────────────────────
        else if (arg == "--input") {
            batchpress::fs::path p = next(i, "--input"); ++i;
            args.process_cfg.input_dir  = p;
            args.video_cfg.input_dir    = p;
            args.scan_cfg.root_dir      = p;
            args.vscan_cfg.root_dir     = p;
        }

        // ── Output ────────────────────────────────────────────────────────
        else if (arg == "--output") {
            batchpress::fs::path p = next(i, "--output"); ++i;
            args.process_cfg.output_dir = p;
            args.video_cfg.output_dir   = p;
        }

        // ── Image options ─────────────────────────────────────────────────
        else if (arg == "--resize") {
            args.process_cfg.resize =
                batchpress::parse_resize(next(i, "--resize")); ++i;
        }
        else if (arg == "--format") {
            args.process_cfg.format =
                batchpress::parse_format(next(i, "--format")); ++i;
        }
        else if (arg == "--quality") {
            args.process_cfg.quality = std::stoi(next(i, "--quality")); ++i;
            if (args.process_cfg.quality < 1 || args.process_cfg.quality > 100)
                throw std::invalid_argument("--quality must be 1–100");
        }

        // ── Video options ─────────────────────────────────────────────────
        else if (arg == "--vcodec") {
            std::string v = next(i, "--vcodec"); ++i;
            if      (v == "h265") args.video_cfg.video_codec = batchpress::VideoCodec::H265;
            else if (v == "h264") args.video_cfg.video_codec = batchpress::VideoCodec::H264;
            else if (v == "vp9")  args.video_cfg.video_codec = batchpress::VideoCodec::VP9;
            else if (v == "auto") args.video_cfg.video_codec = batchpress::VideoCodec::Auto;
            else throw std::invalid_argument("Unknown --vcodec: " + v
                     + ". Valid: h265, h264, vp9, auto");
        }
        else if (arg == "--crf") {
            args.video_cfg.crf = std::stoi(next(i, "--crf")); ++i;
        }
        else if (arg == "--max-res") {
            std::string v = next(i, "--max-res"); ++i;
            if      (v == "1080p")    args.video_cfg.resolution = batchpress::ResolutionCap::Cap1080p;
            else if (v == "4k")       args.video_cfg.resolution = batchpress::ResolutionCap::Cap4K;
            else if (v == "original") args.video_cfg.resolution = batchpress::ResolutionCap::Original;
            else throw std::invalid_argument("Unknown --max-res: " + v
                     + ". Valid: 1080p, 4k, original");
        }

        // ── Common ────────────────────────────────────────────────────────
        else if (arg == "--threads") {
            size_t t = static_cast<size_t>(std::stoul(next(i, "--threads"))); ++i;
            if (t == 0) throw std::invalid_argument("--threads must be >= 1");
            args.process_cfg.num_threads = t;
            args.video_cfg.num_threads   = t;
            args.scan_cfg.num_threads    = t;
            args.vscan_cfg.num_threads   = t;
        }
        else if (arg == "--no-recursive") {
            args.process_cfg.recursive = false;
            args.video_cfg.recursive   = false;
            args.scan_cfg.recursive    = false;
            args.vscan_cfg.recursive   = false;
        }
        else if (arg == "--overwrite") {
            args.process_cfg.skip_existing = false;
            args.video_cfg.skip_existing   = false;
        }
        else if (arg == "--samples") {
            args.scan_cfg.samples_per_dir =
                static_cast<uint32_t>(std::stoul(next(i, "--samples"))); ++i;
        }
        else if (arg == "--verbose") { args.verbose = true; }
        else {
            throw std::invalid_argument("Unknown argument: " + arg
                + "\nRun with --help for usage.");
        }
    }

    // ── Validate ──────────────────────────────────────────────────────────
    auto& input = args.process_cfg.input_dir;
    if (input.empty())
        throw std::runtime_error("--input is required");
    if (!batchpress::fs::exists(input))
        throw std::runtime_error("Input does not exist: " + input.string());
    if (!batchpress::fs::is_directory(input))
        throw std::runtime_error("--input must be a directory: " + input.string());

    if (args.mode == Mode::DryRun) {
        args.process_cfg.dry_run = true;
        args.video_cfg.dry_run   = true;
    }

    return args;
}

} // namespace cli
