// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.marco@gmail.com>
 *
 * This file is part of batchpress — CLI entry point.
 *
 * Routes all operating modes:
 *   Process  → run_batch()       (images) + run_video_batch() (videos)
 *   DryRun   → same, dry_run=true
 *   Scan     → run_scan()        (images only)
 *   ScanVideo→ run_video_scan()  (videos only)
 *   ScanAll  → both scans
 */

#include "cli.hpp"
#include "progress.hpp"
#include "scan_report.hpp"
#include "video_scan_report.hpp"
#include "select.hpp"

#include <batchpress/processor.hpp>
#include <batchpress/scanner.hpp>
#include <batchpress/video_processor.hpp>

extern "C" {
#include <libavutil/avutil.h>
}

#include <iostream>
#include <iomanip>
#include <sstream>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstdlib>
#include <algorithm>

// ── Helpers ───────────────────────────────────────────────────────────────────

// Portable environment variable setter
static void set_env(const char* key, const char* val) {
#ifdef _WIN32
    _putenv_s(key, val);
#else
    setenv(key, val, 0);
#endif
}

static std::string human_bytes(uint64_t b) {
    const char* u[] = {"B","KB","MB","GB","TB"};
    double v = static_cast<double>(b);
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << v << " " << u[i];
    return ss.str();
}

// ── Image process/dry-run report ──────────────────────────────────────────────

static void print_batch_report(const batchpress::BatchReport& r,
                                const std::string& title = "batchpress — Report")
{
    std::cout << "\n┌─────────────────────────────────────────┐\n";
    std::cout << "│  " << std::left << std::setw(39) << title << "│\n";
    std::cout << "├─────────────────────────────────────────┤\n";

    auto row = [](const std::string& l, const std::string& v,
                  const char* c = "\033[0m") {
        std::cout << "│  " << c << std::left << std::setw(18) << l
                  << "\033[0m" << std::right << std::setw(20) << v << "  │\n";
    };

    row("Images", std::to_string(r.total));
    if (!r.dry_run)
        row("Processed", std::to_string(r.succeeded)+"/"+std::to_string(r.total),"\033[32m");
    if (r.skipped)  row("Skipped",  std::to_string(r.skipped));
    if (r.failed)   row("Errors",   std::to_string(r.failed), "\033[31m");
    if (!r.dry_run && (r.written_safe+r.written_direct) > 0) {
        std::cout << "├─────────────────────────────────────────┤\n";
        row("Written (safe)",   std::to_string(r.written_safe),   "\033[32m");
        row("Written (direct)", std::to_string(r.written_direct), "\033[33m");
    }
    std::cout << "├─────────────────────────────────────────┤\n";
    if (!r.dry_run) {
        std::ostringstream t; t << std::fixed << std::setprecision(0) << r.throughput() << " img/s";
        row("Throughput", t.str(), "\033[36m");
    }
    std::ostringstream el; el << std::fixed << std::setprecision(2) << r.elapsed_sec << " s";
    row("Time", el.str());
    if (r.input_bytes_total > 0) {
        std::cout << "├─────────────────────────────────────────┤\n";
        row("Before", human_bytes(r.input_bytes_total));
        row(r.dry_run ? "Projected" : "After",
            human_bytes(r.output_bytes_total), r.dry_run ? "\033[35m" : "\033[0m");
        int64_t sv = r.bytes_saved();
        std::ostringstream s;
        s << human_bytes(static_cast<uint64_t>(sv>0?sv:0))
          << " (" << std::fixed << std::setprecision(1) << r.savings_pct() << "%)";
        row("Saved", s.str(), "\033[32m");
    }
    std::cout << "└─────────────────────────────────────────┘\n";
    if (r.dry_run)
        std::cout << "\n\033[35m  ▸ Run without --dry-run to apply.\033[0m\n";
    std::cout << "\n";
}

// ── Video process/dry-run report ──────────────────────────────────────────────

static void print_video_report(const batchpress::VideoBatchReport& r) {
    std::cout << "\n┌─────────────────────────────────────────┐\n";
    std::cout << "│  batchpress — Video Report                │\n";
    std::cout << "├─────────────────────────────────────────┤\n";

    auto row = [](const std::string& l, const std::string& v,
                  const char* c = "\033[0m") {
        std::cout << "│  " << c << std::left << std::setw(18) << l
                  << "\033[0m" << std::right << std::setw(20) << v << "  │\n";
    };

    row("Videos", std::to_string(r.total));
    if (!r.dry_run)
        row("Processed", std::to_string(r.succeeded)+"/"+std::to_string(r.total),"\033[32m");
    if (r.failed)  row("Errors", std::to_string(r.failed), "\033[31m");

    if (!r.dry_run && (r.used_h265+r.used_h264+r.used_vp9) > 0) {
        std::cout << "├─────────────────────────────────────────┤\n";
        if (r.used_h265) row("H.265 (HEVC)", std::to_string(r.used_h265), "\033[32m");
        if (r.used_h264) row("H.264 (AVC)",  std::to_string(r.used_h264), "\033[33m");
        if (r.used_vp9)  row("VP9",          std::to_string(r.used_vp9),  "\033[36m");
    }

    std::cout << "├─────────────────────────────────────────┤\n";
    std::ostringstream el; el << std::fixed << std::setprecision(2) << r.elapsed_sec << " s";
    row("Time", el.str());

    if (r.input_bytes_total > 0) {
        std::cout << "├─────────────────────────────────────────┤\n";
        row("Before", human_bytes(r.input_bytes_total));
        row(r.dry_run ? "Projected" : "After",
            human_bytes(r.output_bytes_total), r.dry_run ? "\033[35m" : "\033[0m");
        int64_t sv = r.bytes_saved();
        std::ostringstream s;
        s << human_bytes(static_cast<uint64_t>(sv>0?sv:0))
          << " (" << std::fixed << std::setprecision(1) << r.savings_pct() << "%)";
        row("Saved", s.str(), "\033[32m");
    }
    std::cout << "└─────────────────────────────────────────┘\n\n";
}

// ── run_process ───────────────────────────────────────────────────────────────

static int run_process(const cli::Args& args) {
    // Suppress FFmpeg debug/warning logs during processing
    av_log_set_level(AV_LOG_ERROR);
    
    // Suppress x265 logging via environment variable
    set_env("X265_LOG", "quiet");

    // ── Images ────────────────────────────────────────────────────────────
    batchpress::Config img_cfg = args.process_cfg;
    auto img_list = batchpress::collect_images(img_cfg);

    // ── Videos ────────────────────────────────────────────────────────────
    batchpress::VideoConfig vid_cfg = args.video_cfg;

    bool dry = img_cfg.dry_run;

    const char* mode_tag = dry           ? "\033[35mDRY RUN\033[0m"
                         : img_cfg.inplace() ? "\033[33mIN-PLACE\033[0m"
                                             : "\033[36mCOPY\033[0m";

    std::cout << "\033[36m[batchpress]\033[0m " << img_cfg.input_dir.string()
              << "  [" << mode_tag << "]\n\n";

    int exit_code = 0;

    // Process images
    if (!img_list.empty()) {
        std::cout << "\033[36m[batchpress]\033[0m \033[1m" << img_list.size()
                  << "\033[0m image(s)\n";

        auto bar = std::make_shared<cli::ProgressBar>(
            static_cast<uint32_t>(img_list.size()));
        std::mutex console_mu;
        bool verbose = args.verbose;

        img_cfg.on_progress = [&bar, &console_mu, verbose]
            (const batchpress::TaskResult& res, uint32_t, uint32_t)
        {
            bar->tick(res.success || res.skipped);
            if (res.success && res.write_mode == batchpress::WriteMode::Direct
                && !res.original_sha256.empty()) {
                std::lock_guard lock(console_mu);
                std::cout << "\n  \033[33m⚠ DIRECT\033[0m "
                          << res.input_path.filename().string()
                          << "  sha256=" << res.original_sha256;
            }
            if (verbose && !res.skipped) {
                std::lock_guard lock(console_mu);
                if (res.success) {
                    int64_t sv = static_cast<int64_t>(res.input_bytes)
                               - static_cast<int64_t>(res.output_bytes);
                    std::cout << "\n  \033[32m✓\033[0m "
                              << res.input_path.filename().string()
                              << "  " << res.input_bytes/1024 << " KB"
                              << " → " << res.output_bytes/1024 << " KB"
                              << "  (saved " << sv/1024 << " KB)";
                } else {
                    std::cout << "\n  \033[31m✗\033[0m "
                              << res.input_path.filename().string()
                              << ": " << res.error_msg;
                }
            }
        };

        auto img_report = batchpress::run_batch(img_cfg);
        bar->finish();
        print_batch_report(img_report,
            dry ? "batchpress — Image DRY RUN" : "batchpress — Image Report");
        if (img_report.failed > 0) exit_code = 1;
    }

    // Process videos
    {
        // Show video processing settings
        std::cout << "\033[36m[batchpress]\033[0m " << vid_cfg.input_dir.string();
        if (vid_cfg.inplace())
            std::cout << "  \033[33m[IN-PLACE]\033[0m";
        else
            std::cout << "  \033[36m[COPY]\033[0m → " << vid_cfg.output_dir.string();
        std::cout << "\n\n";

        std::atomic<uint32_t> last_files_done{0};
        std::shared_ptr<cli::ProgressBar> vbar;
        std::mutex vbar_mu;
        bool verbose = args.verbose;

        vid_cfg.on_progress = [&](const batchpress::fs::path& path,
                                   uint64_t frame_done, uint64_t frame_total,
                                   uint32_t files_done, uint32_t files_total)
        {
            std::lock_guard lock(vbar_mu);
            if (!vbar && files_total > 0)
                vbar = std::make_shared<cli::ProgressBar>(files_total);

            // Tick once per completed file
            if (vbar && files_done > last_files_done.load()) {
                last_files_done.store(files_done);
                vbar->tick(true);
            }

            if (verbose && frame_total > 0) {
                // Show frame progress inline
                uint32_t pct = static_cast<uint32_t>(
                    frame_done * 100 / frame_total);
                std::cout << "\r  \033[36m" << path.filename().string()
                          << "\033[0m  " << pct << "%   " << std::flush;
            }
        };

        auto vid_report = batchpress::run_video_batch(vid_cfg);
        if (vbar) vbar->finish();
        print_video_report(vid_report);
        if (vid_report.failed > 0) exit_code = 1;
    }

    return exit_code;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    try {
        cli::Args args = cli::parse_args(argc, argv);

        switch (args.mode) {

            case cli::Mode::Scan: {
                batchpress::ScanConfig cfg = args.scan_cfg;
                std::cout << "\033[36m[batchpress]\033[0m Scanning images in \033[1m"
                          << cfg.root_dir << "\033[0m\n\n";
                auto bar = std::make_shared<cli::ProgressBar>(0);
                bool bar_init = false; std::mutex bmu;
                cfg.on_progress = [&](const std::string&, uint32_t /*done*/, uint32_t total) {
                    std::lock_guard lock(bmu);
                    if (!bar_init && total > 0) {
                        bar = std::make_shared<cli::ProgressBar>(total);
                        bar_init = true;
                    }
                    if (bar_init) bar->tick(true);
                };
                auto report = batchpress::run_scan(cfg);
                if (bar_init) bar->finish();
                cli::print_scan_report(report, args.verbose);
                return 0;
            }

            case cli::Mode::ScanVideo: {
                batchpress::VideoScanConfig cfg = args.vscan_cfg;
                std::cout << "\033[36m[batchpress]\033[0m Scanning videos in \033[1m"
                          << cfg.root_dir << "\033[0m\n\n";
                auto bar = std::make_shared<cli::ProgressBar>(0);
                bool bar_init = false; std::mutex bmu;
                cfg.on_progress = [&](const std::string&, uint32_t /*done*/, uint32_t total) {
                    std::lock_guard lock(bmu);
                    if (!bar_init && total > 0) {
                        bar = std::make_shared<cli::ProgressBar>(total);
                        bar_init = true;
                    }
                    if (bar_init) bar->tick(true);
                };
                auto report = batchpress::run_video_scan(cfg);
                if (bar_init) bar->finish();
                cli::print_video_scan_report(report, args.verbose);
                return 0;
            }

            case cli::Mode::ScanAll: {
                // Run both scans sequentially and print both reports
                batchpress::ScanConfig     img_cfg  = args.scan_cfg;
                batchpress::VideoScanConfig vid_cfg = args.vscan_cfg;

                std::cout << "\033[36m[batchpress]\033[0m Full scan: \033[1m"
                          << img_cfg.root_dir << "\033[0m\n\n";

                // Image scan
                std::cout << "\033[90m── Images ──────────────────────────────\033[0m\n";
                auto img_report = batchpress::run_scan(img_cfg);
                cli::print_scan_report(img_report, args.verbose);

                // Video scan
                std::cout << "\033[90m── Videos ──────────────────────────────\033[0m\n";
                auto vid_report = batchpress::run_video_scan(vid_cfg);
                cli::print_video_scan_report(vid_report, args.verbose);

                // Combined total
                int64_t img_saved = img_report.bytes_saved();
                int64_t vid_saved = vid_report.bytes_saved();
                int64_t total_saved = img_saved + vid_saved;
                uint64_t total_current = img_report.total_bytes + vid_report.total_bytes;

                std::cout << "╔══════════════════════════════════════════════════════════╗\n"
                          << "║  COMBINED TOTAL                                          ║\n"
                          << "╠══════════════════════════════════════════════════════════╣\n";
                std::cout << "║  Current total   "
                          << std::right << std::setw(38) << human_bytes(total_current) << "  ║\n";
                std::cout << "║  \033[32mTotal freed\033[0m      "
                          << std::right << std::setw(38)
                          << (human_bytes(static_cast<uint64_t>(total_saved>0?total_saved:0))
                              + "  ("
                              + [&]{ std::ostringstream s;
                                     s << std::fixed << std::setprecision(1)
                                       << (total_current > 0
                                           ? 100.0*(1.0-static_cast<double>(
                                               total_current-total_saved)/total_current)
                                           : 0.0)
                                       << "%)"; return s.str(); }())
                          << "  ║\n";
                std::cout << "╚══════════════════════════════════════════════════════════╝\n\n";
                return 0;
            }

            case cli::Mode::Process:
            case cli::Mode::DryRun:
                return run_process(args);

            case cli::Mode::Select: {
                // Suppress FFmpeg logs
                av_log_set_level(AV_LOG_ERROR);
                set_env("X265_LOG", "quiet");

                std::cout << "\033[36m[batchpress]\033[0m Scanning files in \033[1m"
                          << args.process_cfg.input_dir << "\033[0m\n";
                std::cout << "\033[90m  (analyzing all images and videos...)\033[0m\n\n";

                // Run combined scan (images + videos)
                batchpress::ScanConfig scan_cfg = args.scan_cfg;
                auto img_report = batchpress::scan_files(scan_cfg);

                // Run videoScanReport
                batchpress::VideoScanConfig vscan_cfg = args.vscan_cfg;
                auto vid_report = batchpress::run_video_scan(vscan_cfg);

                // Merge video metadata into FileItem list
                // For now we'll create FileItems from video metadata
                // We need to actually process videos individually to get projections
                // For simplicity in select mode, we'll use the scan report data
                
                // Actually let's properly create FileItems for videos too
                // We'll do a per-file video scan approach
                // For now, let's use a simpler approach: just use image scan + video batch dry-run
                
                // Better approach: collect all videos and create FileItems with estimates
                std::vector<batchpress::FileItem> all_files = std::move(img_report.files);
                
                // Add video items from video scan
                // We need to read individual video metadata
                batchpress::VideoConfig vid_cfg_temp = args.video_cfg;
                vid_cfg_temp.dry_run = true;
                
                // Collect video paths
                std::vector<batchpress::fs::path> video_paths;
                auto add_video = [&](const batchpress::fs::directory_entry& e) {
                    static const std::vector<std::string> video_exts = {
                        ".mp4", ".mov", ".mkv", ".avi", ".webm", ".wmv", ".flv",
                        ".m4v", ".3gp", ".ts", ".mts", ".m2ts"
                    };
                    std::string ext = e.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    for (const auto& ve : video_exts)
                        if (ext == ve && e.is_regular_file()) { video_paths.push_back(e.path()); return; }
                };
                
                if (args.video_cfg.recursive) {
                    for (const auto& e : batchpress::fs::recursive_directory_iterator(
                            args.video_cfg.input_dir, 
                            batchpress::fs::directory_options::skip_permission_denied))
                        add_video(e);
                } else {
                    for (const auto& e : batchpress::fs::directory_iterator(args.video_cfg.input_dir))
                        add_video(e);
                }
                
                // Read metadata for each video and create FileItem
                for (const auto& vp : video_paths) {
                    try {
                        batchpress::VideoMeta vm = batchpress::read_video_meta(vp);
                        batchpress::FileItem fi;
                        fi.type = batchpress::FileItem::Type::Video;
                        fi.path = vp;
                        fi.filename = vp.filename().string();
                        fi.file_size = vm.file_bytes;
                        fi.width = static_cast<uint32_t>(vm.width);
                        fi.height = static_cast<uint32_t>(vm.height);

                        try {
                            fi.last_modified = batchpress::fs::last_write_time(vp);
                            fi.creation_time = fi.last_modified;
                            fi.last_access = fi.last_modified;
                        } catch (...) {}

                        // Video-specific metadata via std::variant
                        batchpress::VideoFileInfo vid_info;
                        vid_info.duration_sec = vm.duration_sec;
                        vid_info.video_codec = vm.video_codec_name;
                        vid_info.audio_codec = vm.audio_codec_name;
                        vid_info.container = vm.container_name;

                        // Compute projected resolution based on resolution cap
                        uint32_t proj_w = static_cast<uint32_t>(vm.width);
                        uint32_t proj_h = static_cast<uint32_t>(vm.height);
                        if (args.video_cfg.resolution == batchpress::ResolutionCap::Cap1080p) {
                            if (vm.height > 1080) {
                                double scale = 1080.0 / vm.height;
                                proj_w = static_cast<uint32_t>(vm.width * scale + 0.5);
                                proj_h = 1080;
                            }
                        } else if (args.video_cfg.resolution == batchpress::ResolutionCap::Cap4K) {
                            if (vm.height > 2160) {
                                double scale = 2160.0 / vm.height;
                                proj_w = static_cast<uint32_t>(vm.width * scale + 0.5);
                                proj_h = 2160;
                            }
                        }
                        vid_info.projected_width  = proj_w;
                        vid_info.projected_height = proj_h;

                        // Estimate savings using CRF factor
                        auto caps = batchpress::probe_codec_caps();
                        auto best_vc = caps.best_video();
                        double crf_factor = 0.50;
                        switch (best_vc) {
                            case batchpress::VideoCodec::H265: crf_factor = 0.40; break;
                            case batchpress::VideoCodec::H264: crf_factor = 0.55; break;
                            case batchpress::VideoCodec::VP9:  crf_factor = 0.45; break;
                            default: break;
                        }

                        fi.projected_size = static_cast<uint64_t>(
                            static_cast<double>(vm.file_bytes) * crf_factor);
                        fi.savings_pct = 100.0 * (1.0 - crf_factor);

                        std::string codec_str;
                        switch (best_vc) {
                            case batchpress::VideoCodec::H265: codec_str = "H.265"; break;
                            case batchpress::VideoCodec::H264: codec_str = "H.264"; break;
                            case batchpress::VideoCodec::VP9:  codec_str = "VP9"; break;
                            default: codec_str = "auto"; break;
                        }
                        vid_info.suggested_codec = codec_str + " CRF" + std::to_string(
                            best_vc == batchpress::VideoCodec::H265 ? 28 :
                            best_vc == batchpress::VideoCodec::H264 ? 26 : 33);

                        fi.meta = std::move(vid_info);
                        all_files.push_back(std::move(fi));
                    } catch (...) {
                        // Skip unreadable videos
                    }
                }
                
                // Create combined report
                batchpress::FileScanReport combined;
                combined.files = std::move(all_files);
                combined.elapsed_sec = img_report.elapsed_sec;
                
                // Sort by savings_pct descending
                std::sort(combined.files.begin(), combined.files.end(),
                    [](const batchpress::FileItem& a, const batchpress::FileItem& b) {
                        return a.savings_pct > b.savings_pct;
                    });
                
                // Now run interactive select UI
                auto select_result = cli::run_select_ui(
                    combined,
                    args.process_cfg,
                    args.video_cfg,
                    args.select_filter,
                    args.select_min_savings
                );
                
                if (select_result.proceed_with_processing && !select_result.selected.empty()) {
                    // Separate images and videos
                    std::vector<batchpress::FileItem> images_to_process;
                    std::vector<batchpress::FileItem> videos_to_process;
                    
                    for (const auto& fi : select_result.selected) {
                        if (fi.type == batchpress::FileItem::Type::Image)
                            images_to_process.push_back(fi);
                        else
                            videos_to_process.push_back(fi);
                    }
                    
                    int exit_code = 0;
                    
                    // Process selected images
                    if (!images_to_process.empty()) {
                        batchpress::Config img_cfg = args.process_cfg;
                        
                        auto bar = std::make_shared<cli::ProgressBar>(
                            static_cast<uint32_t>(images_to_process.size()));
                        std::mutex console_mu;
                        bool verbose = args.verbose;
                        
                        img_cfg.on_progress = [&bar, &console_mu, verbose]
                            (const batchpress::TaskResult& res, uint32_t, uint32_t)
                        {
                            bar->tick(res.success || res.skipped);
                            if (verbose && !res.skipped) {
                                std::lock_guard lock(console_mu);
                                if (res.success) {
                                    std::cout << "\n  \033[32m✓\033[0m "
                                              << res.input_path.filename().string();
                                } else {
                                    std::cout << "\n  \033[31m✗\033[0m "
                                              << res.input_path.filename().string()
                                              << ": " << res.error_msg;
                                }
                            }
                        };
                        
                        auto img_report = batchpress::process_files(images_to_process, img_cfg);
                        bar->finish();
                        print_batch_report(img_report,
                            "batchpress — Selected Images");
                        if (img_report.failed > 0) exit_code = 1;
                    }
                    
                    // Process selected videos
                    if (!videos_to_process.empty()) {
                        batchpress::VideoConfig vid_cfg = args.video_cfg;
                        
                        std::atomic<uint32_t> last_files_done{0};
                        std::shared_ptr<cli::ProgressBar> vbar;
                        std::mutex vbar_mu;
                        bool verbose = args.verbose;
                        
                        vid_cfg.on_progress = [&](const batchpress::fs::path& path,
                                                   uint64_t frame_done, uint64_t frame_total,
                                                   uint32_t files_done, uint32_t files_total)
                        {
                            std::lock_guard lock(vbar_mu);
                            if (!vbar && files_total > 0)
                                vbar = std::make_shared<cli::ProgressBar>(files_total);
                            
                            if (vbar && files_done > last_files_done.load()) {
                                last_files_done.store(files_done);
                                vbar->tick(true);
                            }
                            
                            if (verbose && frame_total > 0) {
                                uint32_t pct = static_cast<uint32_t>(
                                    frame_done * 100 / frame_total);
                                std::cout << "\r  \033[36m" << path.filename().string()
                                          << "\033[0m  " << pct << "%   " << std::flush;
                            }
                        };
                        
                        auto vid_report = batchpress::process_video_files(videos_to_process, vid_cfg);
                        if (vbar) vbar->finish();
                        print_video_report(vid_report);
                        if (vid_report.failed > 0) exit_code = 1;
                    }
                    
                    return exit_code;
                }
                
                return 0;
            }
        }

        return 0;

    } catch (const std::exception& ex) {
        std::cerr << "\n\033[31m[error]\033[0m " << ex.what() << "\n\n";
        return EXIT_FAILURE;
    }
}
