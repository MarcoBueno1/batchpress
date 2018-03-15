// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.mario@gmail.com>
 *
 * This file is part of batchpress — video processor core.
 *
 * DESIGN RULE: No <iostream>, no UI code.
 * All feedback goes through VideoConfig::on_progress.
 * Compiles on Linux, Windows and Android NDK (via ffmpeg-kit).
 */

#include "batchpress/video_processor.hpp"
#include "batchpress/thread_pool.hpp"

// ── libav headers ─────────────────────────────────────────────────────────────
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <unistd.h>

namespace batchpress {

// ── Supported video extensions ────────────────────────────────────────────────

static const std::vector<std::string> VIDEO_EXT = {
    ".mp4", ".mov", ".mkv", ".avi", ".webm", ".wmv",
    ".flv", ".m4v", ".3gp", ".ts",  ".mts",  ".m2ts"
};

static bool is_video(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    for (const auto& e : VIDEO_EXT) if (ext == e) return true;
    return false;
}

// ── CodecCaps ─────────────────────────────────────────────────────────────────

CodecCaps probe_codec_caps() noexcept {
    CodecCaps caps;
    caps.has_h265 = (avcodec_find_encoder_by_name("libx265") != nullptr)
                 || (avcodec_find_encoder_by_name("hevc_nvenc") != nullptr)
                 || (avcodec_find_encoder_by_name("hevc_videotoolbox") != nullptr);
    caps.has_h264 = (avcodec_find_encoder_by_name("libx264") != nullptr)
                 || (avcodec_find_encoder_by_name("h264_nvenc") != nullptr)
                 || (avcodec_find_encoder_by_name("h264_videotoolbox") != nullptr);
    caps.has_vp9  = (avcodec_find_encoder_by_name("libvpx-vp9") != nullptr);
    caps.has_aac  = (avcodec_find_encoder_by_name("aac") != nullptr)
                 || (avcodec_find_encoder_by_name("libfdk_aac") != nullptr);
    caps.has_opus = (avcodec_find_encoder_by_name("libopus") != nullptr);
    return caps;
}

VideoCodec CodecCaps::best_video() const noexcept {
    if (has_h265) return VideoCodec::H265;
    if (has_h264) return VideoCodec::H264;
    if (has_vp9)  return VideoCodec::VP9;
    return VideoCodec::H264; // fallback, will fail gracefully
}

AudioCodec CodecCaps::best_audio(VideoCodec vc) const noexcept {
    if (vc == VideoCodec::VP9 && has_opus) return AudioCodec::Opus;
    if (has_aac)  return AudioCodec::AAC;
    if (has_opus) return AudioCodec::Opus;
    return AudioCodec::None;
}

// ── Codec name lookup ─────────────────────────────────────────────────────────

static const char* video_encoder_name(VideoCodec vc, const CodecCaps& caps) {
    switch (vc) {
        case VideoCodec::H265:
            if (avcodec_find_encoder_by_name("libx265"))          return "libx265";
            if (avcodec_find_encoder_by_name("hevc_nvenc"))       return "hevc_nvenc";
            if (avcodec_find_encoder_by_name("hevc_videotoolbox"))return "hevc_videotoolbox";
            return "libx265";
        case VideoCodec::H264:
            if (avcodec_find_encoder_by_name("libx264"))          return "libx264";
            if (avcodec_find_encoder_by_name("h264_nvenc"))       return "h264_nvenc";
            if (avcodec_find_encoder_by_name("h264_videotoolbox"))return "h264_videotoolbox";
            return "libx264";
        case VideoCodec::VP9:
            return "libvpx-vp9";
        default:
            return video_encoder_name(caps.best_video(), caps);
    }
}

static const char* audio_encoder_name(AudioCodec ac) {
    switch (ac) {
        case AudioCodec::AAC:
            if (avcodec_find_encoder_by_name("libfdk_aac")) return "libfdk_aac";
            return "aac";
        case AudioCodec::Opus:
            return "libopus";
        default:
            return "aac";
    }
}

static const char* container_ext(VideoCodec vc) {
    switch (vc) {
        case VideoCodec::VP9:  return ".webm";
        default:               return ".mp4";
    }
}

// ── Auto CRF selection ────────────────────────────────────────────────────────

static int auto_crf(VideoCodec vc) {
    switch (vc) {
        case VideoCodec::H265: return 28;
        case VideoCodec::H264: return 26;
        case VideoCodec::VP9:  return 33;
        default:               return 28;
    }
}

// ── Resolution cap ────────────────────────────────────────────────────────────

static std::pair<int,int> apply_res_cap(int w, int h, ResolutionCap cap) {
    int max_h = 0;
    switch (cap) {
        case ResolutionCap::Cap1080p: max_h = 1080; break;
        case ResolutionCap::Cap4K:    max_h = 2160; break;
        default: return {w, h};
    }
    if (h <= max_h) return {w, h};
    // Scale down keeping aspect ratio, width must be divisible by 2
    double scale = static_cast<double>(max_h) / h;
    int nw = static_cast<int>(w * scale);
    int nh = max_h;
    nw = (nw % 2 == 0) ? nw : nw - 1;
    return {nw, nh};
}

// ── Audio content classification ──────────────────────────────────────────────
//
// Samples decoded audio frames and measures RMS energy + spectral centroid
// to classify as Speech, Music or Silent.
// This is a lightweight heuristic — not ML-based.

static AudioContent classify_audio(AVFormatContext* fmt_ctx, int audio_stream_idx) {
    if (audio_stream_idx < 0) return AudioContent::Unknown;

    AVStream*  stream  = fmt_ctx->streams[audio_stream_idx];
    const AVCodec* dec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!dec) return AudioContent::Unknown;

    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx) return AudioContent::Unknown;

    avcodec_parameters_to_context(ctx, stream->codecpar);
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return AudioContent::Unknown;
    }

    AVPacket* pkt   = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();

    double rms_sum   = 0.0;
    int64_t n_samples= 0;
    int     n_frames = 0;
    const   int MAX_FRAMES = 100; // sample first ~100 audio frames

    while (n_frames < MAX_FRAMES
           && av_read_frame(fmt_ctx, pkt) >= 0)
    {
        if (pkt->stream_index != audio_stream_idx) {
            av_packet_unref(pkt);
            continue;
        }

        if (avcodec_send_packet(ctx, pkt) == 0) {
            while (avcodec_receive_frame(ctx, frame) == 0) {
                // Compute RMS over all samples in this frame
                int samples = frame->nb_samples
                            * frame->ch_layout.nb_channels;
                if (frame->format == AV_SAMPLE_FMT_FLTP
                 || frame->format == AV_SAMPLE_FMT_FLT)
                {
                    float* data = reinterpret_cast<float*>(frame->data[0]);
                    for (int i = 0; i < samples; ++i)
                        rms_sum += static_cast<double>(data[i]) * data[i];
                    n_samples += samples;
                }
                av_frame_unref(frame);
                ++n_frames;
            }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);

    // Seek back to beginning
    av_seek_frame(fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);

    if (n_samples == 0) return AudioContent::Silent;

    double rms = std::sqrt(rms_sum / static_cast<double>(n_samples));

    if (rms < 0.001) return AudioContent::Silent;  // essentially silent
    if (rms < 0.05)  return AudioContent::Speech;  // low energy → speech
    return AudioContent::Music;                     // high energy → music
}

// ── Auto audio bitrate ────────────────────────────────────────────────────────

static int auto_audio_bitrate(AudioContent content, AudioCodec codec) {
    switch (content) {
        case AudioContent::Silent:  return 0;     // remove audio
        case AudioContent::Speech:  return (codec == AudioCodec::Opus) ? 48  : 64;
        case AudioContent::Music:   return (codec == AudioCodec::Opus) ? 96  : 128;
        default:                    return (codec == AudioCodec::Opus) ? 96  : 128;
    }
}

// ── collect_videos ────────────────────────────────────────────────────────────

static std::vector<fs::path> collect_videos(const VideoConfig& cfg) {
    std::vector<fs::path> result;

    auto add = [&](const fs::directory_entry& e) {
        if (e.is_regular_file() && is_video(e.path()))
            result.push_back(e.path());
    };

    if (cfg.recursive) {
        for (const auto& e : fs::recursive_directory_iterator(
                cfg.input_dir, fs::directory_options::skip_permission_denied))
            add(e);
    } else {
        for (const auto& e : fs::directory_iterator(cfg.input_dir))
            add(e);
    }

    std::sort(result.begin(), result.end());
    return result;
}

// ── read_video_meta ───────────────────────────────────────────────────────────

VideoMeta read_video_meta(const fs::path& path) {
    VideoMeta m;
    m.path       = path;
    m.file_bytes = fs::exists(path) ? fs::file_size(path) : 0;

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.string().c_str(), nullptr, nullptr) < 0)
        return m;

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return m;
    }

    m.duration_sec = (fmt->duration > 0)
        ? static_cast<double>(fmt->duration) / AV_TIME_BASE
        : 0.0;

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream*          s  = fmt->streams[i];
        AVCodecParameters* cp = s->codecpar;

        if (cp->codec_type == AVMEDIA_TYPE_VIDEO && m.width == 0) {
            m.width             = cp->width;
            m.height            = cp->height;
            m.video_bitrate     = cp->bit_rate;
            m.video_codec_name  = avcodec_get_name(cp->codec_id);

            if (s->avg_frame_rate.den > 0)
                m.fps = av_q2d(s->avg_frame_rate);

            if (m.fps > 0 && m.duration_sec > 0)
                m.frame_count = static_cast<uint64_t>(m.fps * m.duration_sec);
        }
        else if (cp->codec_type == AVMEDIA_TYPE_AUDIO && m.audio_codec_name.empty()) {
            m.audio_bitrate    = cp->bit_rate;
            m.audio_codec_name = avcodec_get_name(cp->codec_id);
        }
    }

    if (fmt->iformat) m.container_name = fmt->iformat->name;
    m.readable = (m.width > 0);

    avformat_close_input(&fmt);
    return m;
}

// ── RAII wrappers for libav contexts ─────────────────────────────────────────

struct FmtGuard {
    AVFormatContext* p = nullptr;
    explicit FmtGuard(AVFormatContext* ctx) : p(ctx) {}
    ~FmtGuard() { if (p) avformat_close_input(&p); }
    FmtGuard(const FmtGuard&) = delete;
};

struct OutFmtGuard {
    AVFormatContext* p = nullptr;
    explicit OutFmtGuard(AVFormatContext* ctx) : p(ctx) {}
    ~OutFmtGuard() {
        if (p) {
            if (!(p->oformat->flags & AVFMT_NOFILE))
                avio_closep(&p->pb);
            avformat_free_context(p);
        }
    }
    OutFmtGuard(const OutFmtGuard&) = delete;
};

struct CodecCtxGuard {
    AVCodecContext* p = nullptr;
    explicit CodecCtxGuard(AVCodecContext* ctx) : p(ctx) {}
    ~CodecCtxGuard() { if (p) avcodec_free_context(&p); }
    CodecCtxGuard(const CodecCtxGuard&) = delete;
};

struct FrameGuard {
    AVFrame* p = nullptr;
    explicit FrameGuard(AVFrame* f) : p(f) {}
    ~FrameGuard() { if (p) av_frame_free(&p); }
    FrameGuard(const FrameGuard&) = delete;
};

struct PacketGuard {
    AVPacket* p = nullptr;
    explicit PacketGuard(AVPacket* pk) : p(pk) {}
    ~PacketGuard() { if (p) av_packet_free(&p); }
    PacketGuard(const PacketGuard&) = delete;
};

// ── process_video ─────────────────────────────────────────────────────────────

VideoResult process_video(const fs::path& input_path, const VideoConfig& cfg) {
    using Clock = std::chrono::high_resolution_clock;
    auto t0 = Clock::now();

    VideoResult result;
    result.input_path  = input_path;
    result.dry_run     = cfg.dry_run;
    result.input_bytes = fs::exists(input_path) ? fs::file_size(input_path) : 0;

    // For x265 log suppression
    int old_stdout = -1, old_stderr = -1, devnull = -1;

    try {
        // ── 1. Probe runtime codec capabilities ───────────────────────────
        static const CodecCaps caps = probe_codec_caps();

        VideoCodec vcodec = (cfg.video_codec == VideoCodec::Auto)
            ? caps.best_video() : cfg.video_codec;
        AudioCodec acodec = (cfg.audio_codec == AudioCodec::Auto)
            ? caps.best_audio(vcodec) : cfg.audio_codec;

        int crf = (cfg.crf < 0) ? auto_crf(vcodec) : cfg.crf;

        result.codec_used = vcodec;
        result.audio_used = acodec;
        result.crf_used   = crf;

        // ── 2. Output path ────────────────────────────────────────────────
        fs::path out_path = cfg.inplace()
            ? input_path
            : cfg.output_dir / fs::relative(input_path, cfg.input_dir);

        out_path.replace_extension(container_ext(vcodec));
        result.output_path = out_path;

        // ── 3. Skip check ─────────────────────────────────────────────────
        if (cfg.skip_existing && !cfg.inplace() && fs::exists(out_path)) {
            result.success      = true;
            result.skipped      = true;
            result.output_bytes = fs::file_size(out_path);
            result.elapsed_sec  = std::chrono::duration<double>(
                                      Clock::now() - t0).count();
            return result;
        }

        // ── 4. Open input ─────────────────────────────────────────────────
        AVFormatContext* in_fmt_raw = nullptr;
        if (avformat_open_input(&in_fmt_raw,
                input_path.string().c_str(), nullptr, nullptr) < 0)
            throw std::runtime_error("Cannot open input: " + input_path.string());
        FmtGuard in_fmt(in_fmt_raw);

        if (avformat_find_stream_info(in_fmt.p, nullptr) < 0)
            throw std::runtime_error("Cannot read stream info");

        // Find video and audio stream indices
        int v_idx = av_find_best_stream(in_fmt.p, AVMEDIA_TYPE_VIDEO,
                                        -1, -1, nullptr, 0);
        int a_idx = av_find_best_stream(in_fmt.p, AVMEDIA_TYPE_AUDIO,
                                        -1, -1, nullptr, 0);

        if (v_idx < 0)
            throw std::runtime_error("No video stream found");

        // ── 5. Classify audio content ─────────────────────────────────────
        AudioContent audio_content = classify_audio(in_fmt.p, a_idx);
        result.audio_content = audio_content;

        int audio_bps = (cfg.audio_bitrate_kbps > 0)
            ? cfg.audio_bitrate_kbps
            : auto_audio_bitrate(audio_content, acodec);

        if (audio_content == AudioContent::Silent || audio_bps == 0)
            acodec = AudioCodec::None;

        // ── 6. Dry-run estimate ───────────────────────────────────────────
        if (cfg.dry_run) {
            // Estimate based on CRF and resolution cap
            AVStream* vs = in_fmt.p->streams[v_idx];
            int src_w = vs->codecpar->width;
            int src_h = vs->codecpar->height;
            auto [dst_w, dst_h] = apply_res_cap(src_w, src_h, cfg.resolution);

            double res_ratio  = static_cast<double>(dst_w * dst_h)
                              / static_cast<double>(src_w * src_h);

            // CRF compression estimate (empirical)
            double crf_factor = 1.0;
            switch (vcodec) {
                case VideoCodec::H265: crf_factor = 0.40; break;
                case VideoCodec::H264: crf_factor = 0.55; break;
                case VideoCodec::VP9:  crf_factor = 0.45; break;
                default:               crf_factor = 0.50; break;
            }

            result.output_bytes = static_cast<uint64_t>(
                result.input_bytes * res_ratio * crf_factor);
            result.success      = true;
            result.elapsed_sec  = std::chrono::duration<double>(
                                      Clock::now() - t0).count();
            return result;
        }

        // ── 7. Open video decoder ─────────────────────────────────────────
        AVStream* v_stream = in_fmt.p->streams[v_idx];
        const AVCodec* v_dec = avcodec_find_decoder(v_stream->codecpar->codec_id);
        if (!v_dec) throw std::runtime_error("No video decoder available");

        // Get dimensions and pixel format from codecpar BEFORE opening decoder
        int src_width  = v_stream->codecpar->width;
        int src_height = v_stream->codecpar->height;
        AVPixelFormat src_pix_fmt = static_cast<AVPixelFormat>(v_stream->codecpar->format);

        // Validate video dimensions early
        if (src_width <= 0 || src_height <= 0) {
            throw std::runtime_error(
                "Invalid video dimensions: " + std::to_string(src_width) + "x"
                + std::to_string(src_height) + " in " + input_path.string());
        }
        if (src_pix_fmt == AV_PIX_FMT_NONE) {
            throw std::runtime_error(
                "Invalid video pixel format in " + input_path.string());
        }

        CodecCtxGuard v_dec_ctx(avcodec_alloc_context3(v_dec));
        avcodec_parameters_to_context(v_dec_ctx.p, v_stream->codecpar);
        v_dec_ctx.p->thread_count = 2;
        if (avcodec_open2(v_dec_ctx.p, v_dec, nullptr) < 0)
            throw std::runtime_error("Cannot open video decoder");

        // ── 8. Open audio decoder ─────────────────────────────────────────
        AVCodecContext* a_dec_ctx_raw = nullptr;
        if (a_idx >= 0 && acodec != AudioCodec::None) {
            AVStream* a_stream = in_fmt.p->streams[a_idx];
            const AVCodec* a_dec = avcodec_find_decoder(a_stream->codecpar->codec_id);
            if (a_dec) {
                a_dec_ctx_raw = avcodec_alloc_context3(a_dec);
                avcodec_parameters_to_context(a_dec_ctx_raw, a_stream->codecpar);
                if (avcodec_open2(a_dec_ctx_raw, a_dec, nullptr) < 0) {
                    avcodec_free_context(&a_dec_ctx_raw);
                    a_dec_ctx_raw = nullptr;
                }
            }
        }
        CodecCtxGuard a_dec_ctx(a_dec_ctx_raw);

        // ── 9. Decide write strategy ──────────────────────────────────────
        if (!cfg.inplace())
            fs::create_directories(out_path.parent_path());

        // For videos, we always use temp file + rename for safety.
        // Unlike images, video encoding requires reading input while writing output,
        // so true "Direct" mode (overwriting the same file) is not feasible without
        // complex AVIO custom buffering. BATCHPRESS_FORCE_DIRECT has no effect for videos.
        fs::path write_path = cfg.inplace()
            ? fs::path(out_path.string() + ".batchpress.tmp")
            : out_path;

        const char* format_name = cfg.inplace() ? container_ext(vcodec) + 1 : nullptr;

        AVFormatContext* out_fmt_raw = nullptr;
        if (avformat_alloc_output_context2(&out_fmt_raw, nullptr,
                format_name, write_path.string().c_str()) < 0)
            throw std::runtime_error("Cannot create output context");
        OutFmtGuard out_fmt(out_fmt_raw);

        // ── 10. Video encoder ─────────────────────────────────────────────
        const char* v_enc_name = video_encoder_name(vcodec, caps);
        const AVCodec* v_enc   = avcodec_find_encoder_by_name(v_enc_name);
        if (!v_enc) throw std::runtime_error(
            std::string("Video encoder not found: ") + v_enc_name);

        AVStream* out_v_stream = avformat_new_stream(out_fmt.p, nullptr);
        CodecCtxGuard v_enc_ctx(avcodec_alloc_context3(v_enc));

        // Set private options FIRST, before any other configuration
        av_opt_set(v_enc_ctx.p->priv_data, "preset", "ultrafast", 0);
        av_opt_set_int(v_enc_ctx.p->priv_data, "crf", crf, 0);

        // Use validated dimensions from decoder
        auto [dst_w, dst_h] = apply_res_cap(src_width, src_height, cfg.resolution);

        // Set encoder context parameters
        v_enc_ctx.p->width      = dst_w;
        v_enc_ctx.p->height     = dst_h;
        v_enc_ctx.p->pix_fmt    = AV_PIX_FMT_YUV420P;
        v_enc_ctx.p->framerate  = v_stream->avg_frame_rate;
        
        // Use stream time_base or 1/90000 for encoder (standard for MPEG)
        v_enc_ctx.p->time_base  = v_stream->time_base;
        if (v_enc_ctx.p->time_base.num <= 0 || v_enc_ctx.p->time_base.den <= 0)
            v_enc_ctx.p->time_base = (AVRational){1, 90000};
        
        v_enc_ctx.p->thread_count = 4;  // Limit threads to avoid concurrency issues

        // Also set stream parameters
        out_v_stream->time_base = v_enc_ctx.p->time_base;

        // VP9-specific options
        if (vcodec == VideoCodec::VP9) {
            // deadline: 0=best quality, 1=good, 2=realtime
            av_opt_set_int(v_enc_ctx.p->priv_data, "deadline", 0, 0);
            av_opt_set_int(v_enc_ctx.p->priv_data, "cpu-used", 2, 0);
        }

        if (out_fmt.p->oformat->flags & AVFMT_GLOBALHEADER)
            v_enc_ctx.p->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        // Open encoder with suppressed output (x265 logs to stdout/stderr)
        {
            // Temporarily suppress stdout/stderr to hide x265 init logs
            int old_stdout = dup(STDOUT_FILENO);
            int old_stderr = dup(STDERR_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            
            int ret = avcodec_open2(v_enc_ctx.p, v_enc, nullptr);
            
            // Restore stdout/stderr
            dup2(old_stdout, STDOUT_FILENO);
            dup2(old_stderr, STDERR_FILENO);
            close(old_stdout);
            close(old_stderr);
            close(devnull);
            
            if (ret < 0) {
                char err_buf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, err_buf, sizeof(err_buf));
                throw std::runtime_error("Cannot open video encoder: " + std::string(err_buf));
            }
        }

        avcodec_parameters_from_context(out_v_stream->codecpar, v_enc_ctx.p);
        out_v_stream->time_base = v_enc_ctx.p->time_base;

        // ── 11. Audio encoder ─────────────────────────────────────────────
        AVCodecContext* a_enc_ctx_raw = nullptr;
        AVStream*       out_a_stream  = nullptr;
        SwrContext*     swr_raw        = nullptr;

        if (acodec != AudioCodec::None && a_dec_ctx.p) {
            const AVCodec* a_enc = avcodec_find_encoder_by_name(
                audio_encoder_name(acodec));
            if (a_enc) {
                a_enc_ctx_raw = avcodec_alloc_context3(a_enc);
                a_enc_ctx_raw->sample_rate  = a_dec_ctx.p->sample_rate;
                a_enc_ctx_raw->ch_layout    = a_dec_ctx.p->ch_layout;

                // Get supported sample formats via new API (avcodec_get_supported_config)
                // Fallback to FLTP which is universally supported
                const AVSampleFormat* sup_fmts = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 0, 0)
                int nb_fmts = 0;
                if (avcodec_get_supported_config(nullptr, a_enc, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                        0, (const void**)&sup_fmts, &nb_fmts) == 0 && nb_fmts > 0)
                    a_enc_ctx_raw->sample_fmt = sup_fmts[0];
                else
                    a_enc_ctx_raw->sample_fmt = AV_SAMPLE_FMT_FLTP;
#else
                // Older libavcodec: use the deprecated field directly
                #pragma GCC diagnostic push
                #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                a_enc_ctx_raw->sample_fmt = (a_enc->sample_fmts && a_enc->sample_fmts[0] != AV_SAMPLE_FMT_NONE)
                    ? a_enc->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
                #pragma GCC diagnostic pop
#endif

                a_enc_ctx_raw->bit_rate     = audio_bps * 1000LL;
                a_enc_ctx_raw->time_base    = {1, a_dec_ctx.p->sample_rate};

                if (out_fmt.p->oformat->flags & AVFMT_GLOBALHEADER)
                    a_enc_ctx_raw->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

                if (avcodec_open2(a_enc_ctx_raw, a_enc, nullptr) == 0) {
                    out_a_stream = avformat_new_stream(out_fmt.p, nullptr);
                    avcodec_parameters_from_context(
                        out_a_stream->codecpar, a_enc_ctx_raw);
                    out_a_stream->time_base = a_enc_ctx_raw->time_base;

                    // Resampler (handles format/rate conversion)
                    swr_alloc_set_opts2(&swr_raw,
                        &a_enc_ctx_raw->ch_layout,
                         a_enc_ctx_raw->sample_fmt,
                         a_enc_ctx_raw->sample_rate,
                        &a_dec_ctx.p->ch_layout,
                         a_dec_ctx.p->sample_fmt,
                         a_dec_ctx.p->sample_rate,
                        0, nullptr);
                    swr_init(swr_raw);
                } else {
                    avcodec_free_context(&a_enc_ctx_raw);
                }
            }
        }

        CodecCtxGuard a_enc_ctx(a_enc_ctx_raw);

        // ── 12. Scaler (pixel format + resize) ────────────────────────────
        SwsContext* sws = sws_getContext(
            src_width, src_height, src_pix_fmt,
            dst_w, dst_h, AV_PIX_FMT_YUV420P,
            SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!sws) throw std::runtime_error("Cannot create scaler");

        // ── 13. Open output file and write header ─────────────────────────
        if (!(out_fmt.p->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&out_fmt.p->pb,
                          write_path.string().c_str(), AVIO_FLAG_WRITE) < 0)
                throw std::runtime_error(
                    "Cannot open output file: " + write_path.string());
        }

        if (avformat_write_header(out_fmt.p, nullptr) < 0)
            throw std::runtime_error("Cannot write output header");

        // ── 14. Encode loop ───────────────────────────────────────────────
        // Suppress x265 encoding statistics
        old_stdout = dup(STDOUT_FILENO);
        old_stderr = dup(STDERR_FILENO);
        devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        
        PacketGuard in_pkt(av_packet_alloc());
        FrameGuard  dec_frame(av_frame_alloc());
        FrameGuard  sws_frame(av_frame_alloc());

        sws_frame.p->format = AV_PIX_FMT_YUV420P;
        sws_frame.p->width  = dst_w;
        sws_frame.p->height = dst_h;
        av_frame_get_buffer(sws_frame.p, 0);

        uint64_t frame_count = 0;
        uint64_t total_frames = 0;
        if (v_stream->nb_frames > 0)
            total_frames = static_cast<uint64_t>(v_stream->nb_frames);

        // Encode helper lambda
        auto flush_encoder = [&](AVCodecContext* enc_ctx,
                                  AVStream* out_stream,
                                  AVFrame* frame)
        {
            if (avcodec_send_frame(enc_ctx, frame) < 0) return;
            PacketGuard out_pkt(av_packet_alloc());
            while (avcodec_receive_packet(enc_ctx, out_pkt.p) == 0) {
                av_packet_rescale_ts(out_pkt.p,
                    enc_ctx->time_base, out_stream->time_base);
                out_pkt.p->stream_index = out_stream->index;
                av_interleaved_write_frame(out_fmt.p, out_pkt.p);
                av_packet_unref(out_pkt.p);
            }
        };

        while (av_read_frame(in_fmt.p, in_pkt.p) >= 0) {
            if (in_pkt.p->stream_index == v_idx) {
                // Video frame
                if (avcodec_send_packet(v_dec_ctx.p, in_pkt.p) == 0) {
                    while (avcodec_receive_frame(v_dec_ctx.p, dec_frame.p) == 0) {
                        // Scale / convert pixel format
                        sws_scale(sws,
                            dec_frame.p->data, dec_frame.p->linesize,
                            0, src_height,
                            sws_frame.p->data, sws_frame.p->linesize);

                        sws_frame.p->pts = dec_frame.p->pts;
                        flush_encoder(v_enc_ctx.p, out_v_stream, sws_frame.p);

                        ++frame_count;
                        if (cfg.on_progress)
                            cfg.on_progress(input_path, frame_count,
                                            total_frames, 0, 0);

                        av_frame_unref(dec_frame.p);
                    }
                }
            }
            else if (in_pkt.p->stream_index == a_idx
                     && a_enc_ctx.p && out_a_stream && swr_raw)
            {
                // Audio frame
                if (avcodec_send_packet(a_dec_ctx.p, in_pkt.p) == 0) {
                    while (avcodec_receive_frame(a_dec_ctx.p, dec_frame.p) == 0) {
                        FrameGuard swr_frame(av_frame_alloc());
                        swr_frame.p->sample_rate    = a_enc_ctx.p->sample_rate;
                        swr_frame.p->ch_layout      = a_enc_ctx.p->ch_layout;
                        swr_frame.p->format         = a_enc_ctx.p->sample_fmt;
                        swr_frame.p->nb_samples     = a_enc_ctx.p->frame_size > 0
                            ? a_enc_ctx.p->frame_size : dec_frame.p->nb_samples;
                        av_frame_get_buffer(swr_frame.p, 0);

                        swr_convert_frame(swr_raw, swr_frame.p, dec_frame.p);
                        swr_frame.p->pts = dec_frame.p->pts;

                        flush_encoder(a_enc_ctx.p, out_a_stream, swr_frame.p);
                        av_frame_unref(dec_frame.p);
                    }
                }
            }
            av_packet_unref(in_pkt.p);
        }

        // Flush encoders
        flush_encoder(v_enc_ctx.p, out_v_stream, nullptr);
        if (a_enc_ctx.p && out_a_stream)
            flush_encoder(a_enc_ctx.p, out_a_stream, nullptr);

        av_write_trailer(out_fmt.p);

        // Cleanup scaler and resampler
        sws_freeContext(sws);
        if (swr_raw) swr_free(&swr_raw);

        // ── 15. In-place atomic rename ────────────────────────────────────
        if (cfg.inplace())
            fs::rename(write_path, out_path);

        result.success      = true;
        result.output_bytes = fs::file_size(out_path);

    } catch (const std::exception& ex) {
        result.success   = false;
        result.error_msg = ex.what();
    }

    // Restore stdout/stderr AFTER all encoding and cleanup is complete
    if (devnull >= 0) {
        dup2(old_stdout, STDOUT_FILENO);
        dup2(old_stderr, STDERR_FILENO);
        close(old_stdout);
        close(old_stderr);
        close(devnull);
    }

    result.elapsed_sec = std::chrono::duration<double>(
                             Clock::now() - Clock::time_point{}).count();
    return result;
}

// ── VideoBatchReport helpers ──────────────────────────────────────────────────

double  VideoBatchReport::throughput()   const noexcept {
    return elapsed_sec > 0.0 ? succeeded / elapsed_sec : 0.0;
}
int64_t VideoBatchReport::bytes_saved()  const noexcept {
    return static_cast<int64_t>(input_bytes_total)
         - static_cast<int64_t>(output_bytes_total);
}
double  VideoBatchReport::savings_pct()  const noexcept {
    if (input_bytes_total == 0) return 0.0;
    return 100.0 * (1.0 - static_cast<double>(output_bytes_total)
                           / static_cast<double>(input_bytes_total));
}

// ── VideoScanReport helpers ───────────────────────────────────────────────────

int64_t VideoScanReport::bytes_saved() const noexcept {
    return static_cast<int64_t>(total_bytes)
         - static_cast<int64_t>(projected_bytes);
}
double VideoScanReport::savings_pct() const noexcept {
    if (total_bytes == 0) return 0.0;
    return 100.0 * (1.0 - static_cast<double>(projected_bytes)
                           / static_cast<double>(total_bytes));
}

std::string VideoDirScanResult::suggested_command(const std::string& exe) const {
    std::ostringstream ss;
    ss << exe << " --input \"" << directory.string() << "\"";
    switch (suggested_codec) {
        case VideoCodec::H265: ss << " --vcodec h265 --crf " << suggested_crf; break;
        case VideoCodec::H264: ss << " --vcodec h264 --crf " << suggested_crf; break;
        case VideoCodec::VP9:  ss << " --vcodec vp9  --crf " << suggested_crf; break;
        default: break;
    }
    if (suggested_res == ResolutionCap::Cap1080p) ss << " --max-res 1080p";
    return ss.str();
}

std::string VideoScanReport::suggested_command(const std::string& exe) const {
    if (directories.empty()) return "";
    // Use the suggestion from the directory with highest savings
    return directories.front().suggested_command(exe);
}

// ── run_video_batch ───────────────────────────────────────────────────────────

VideoBatchReport run_video_batch(const VideoConfig& cfg) {
    using Clock = std::chrono::steady_clock;

    auto videos = collect_videos(cfg);
    if (videos.empty()) return {};

    if (!cfg.inplace() && !cfg.dry_run)
        fs::create_directories(cfg.output_dir);

    VideoBatchReport report;
    report.total   = static_cast<uint32_t>(videos.size());
    report.dry_run = cfg.dry_run;

    std::mutex      report_mu;
    std::atomic<uint32_t> files_done{0};

    size_t threads = cfg.num_threads > 0
        ? cfg.num_threads
        : std::max(size_t(1),
                   size_t(std::thread::hardware_concurrency() / 2));

    ThreadPool pool(threads);
    auto t0 = Clock::now();

    // Wrap per-file progress to inject files_done/total
    auto make_cfg = [&](const fs::path& /*path*/) -> VideoConfig {
        VideoConfig c = cfg;
        uint32_t fd = files_done.load(std::memory_order_relaxed);
        if (cfg.on_progress) {
            c.on_progress = [&cfg, fd, &report](
                const fs::path& p, uint64_t fdone, uint64_t ftotal,
                uint32_t, uint32_t)
            {
                if (cfg.on_progress)
                    cfg.on_progress(p, fdone, ftotal, fd, report.total);
            };
        }
        return c;
    };

    std::vector<std::future<VideoResult>> futures;
    futures.reserve(videos.size());
    for (const auto& v : videos)
        futures.push_back(pool.submit(process_video, v, make_cfg(v)));

    for (auto& fut : futures) {
        VideoResult res = fut.get();
        files_done.fetch_add(1, std::memory_order_relaxed);

        std::lock_guard lock(report_mu);
        if (res.skipped) {
            ++report.skipped;
        } else if (res.success) {
            ++report.succeeded;
            report.input_bytes_total  += res.input_bytes;
            report.output_bytes_total += res.output_bytes;
            switch (res.codec_used) {
                case VideoCodec::H265: ++report.used_h265; break;
                case VideoCodec::H264: ++report.used_h264; break;
                case VideoCodec::VP9:  ++report.used_vp9;  break;
                default: break;
            }
        } else {
            ++report.failed;
        }
    }

    report.elapsed_sec = std::chrono::duration<double>(
                             Clock::now() - t0).count();
    return report;
}

// ── run_video_scan ────────────────────────────────────────────────────────────

VideoScanReport run_video_scan(const VideoScanConfig& cfg) {
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();

    VideoScanReport report;
    report.root_dir = cfg.root_dir;
    report.caps     = probe_codec_caps();

    // Group videos by directory
    std::map<fs::path, std::vector<fs::path>> groups;
    auto add = [&](const fs::directory_entry& e) {
        if (e.is_regular_file() && is_video(e.path()))
            groups[e.path().parent_path()].push_back(e.path());
    };

    if (cfg.recursive) {
        for (const auto& e : fs::recursive_directory_iterator(
                cfg.root_dir, fs::directory_options::skip_permission_denied))
            add(e);
    } else {
        for (const auto& e : fs::directory_iterator(cfg.root_dir))
            add(e);
    }

    uint32_t total = 0;
    for (const auto& [d, vs] : groups) total += static_cast<uint32_t>(vs.size());
    std::atomic<uint32_t> done{0};

    size_t threads = cfg.num_threads > 0
        ? cfg.num_threads : std::thread::hardware_concurrency();
    ThreadPool pool(threads);

    using FutureDir = std::future<VideoDirScanResult>;
    std::vector<FutureDir> futures;
    futures.reserve(groups.size());

    for (const auto& [dir, videos] : groups) {
        futures.push_back(pool.submit(
            [&report, &cfg, &done, total]
            (fs::path d, std::vector<fs::path> vids) -> VideoDirScanResult
            {
                VideoDirScanResult res;
                res.directory    = d;
                res.video_count  = static_cast<uint32_t>(vids.size());

                VideoCodec best_vc = report.caps.best_video();
                int        best_crf= auto_crf(best_vc);

                for (const auto& vp : vids) {
                    VideoMeta m = read_video_meta(vp);
                    if (!m.readable) continue;

                    res.total_bytes         += m.file_bytes;
                    res.total_duration_sec  += static_cast<uint64_t>(m.duration_sec);

                    uint32_t n = done.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (cfg.on_progress)
                        cfg.on_progress(vp.filename().string(), n, total);
                }

                // Estimate: CRF + resolution cap
                ResolutionCap rcap = ResolutionCap::Cap1080p;
                double crf_factor = 0.0;
                switch (best_vc) {
                    case VideoCodec::H265: crf_factor = 0.40; break;
                    case VideoCodec::H264: crf_factor = 0.55; break;
                    case VideoCodec::VP9:  crf_factor = 0.45; break;
                    default:               crf_factor = 0.50; break;
                }

                res.projected_bytes   = static_cast<uint64_t>(
                    res.total_bytes * crf_factor);
                res.savings_pct       = 100.0 * (1.0 - crf_factor);
                res.suggested_codec   = best_vc;
                res.suggested_crf     = best_crf;
                res.suggested_res     = rcap;

                return res;
            },
            dir, videos
        ));
    }

    report.directories.reserve(groups.size());
    for (auto& fut : futures) {
        VideoDirScanResult dr = fut.get();
        report.total_videos   += dr.video_count;
        report.total_bytes    += dr.total_bytes;
        report.projected_bytes+= dr.projected_bytes;
        report.directories.push_back(std::move(dr));
    }

    // Sort by bytes saved descending
    std::sort(report.directories.begin(), report.directories.end(),
        [](const VideoDirScanResult& a, const VideoDirScanResult& b) {
            return a.bytes_saved() > b.bytes_saved();
        });

    report.elapsed_sec = std::chrono::duration<double>(
                             Clock::now() - t0).count();
    return report;
}

} // namespace batchpress
