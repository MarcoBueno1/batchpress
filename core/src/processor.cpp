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

/*
 * DESIGN RULE: This file must never include <iostream>, <cstdio> or any
 * platform-specific UI header. All feedback to the caller goes exclusively
 * through Config::on_progress. This is what makes the core portable to
 * Android NDK, Qt, CLI and any future UI without modification.
 */

#include "batchpress/processor.hpp"
#include "batchpress/thread_pool.hpp"

// ── stb single-file libraries ─────────────────────────────────────────────────
// STB_IMAGE_STATIC makes all stb symbols have internal linkage (static).
// This allows including stb in multiple translation units without linker
// conflicts. Each TU that needs stb must define these macros before including.
//
// Implementations defined here and in scanner.cpp independently (static).
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STB_IMAGE_WRITE_STATIC
#define STBIRDEF static          // makes stb_image_resize2 symbols static too
#define STBI_FAILURE_USERMSG

#include "../third_party/stb_image.h"
#include "../third_party/stb_image_write.h"
#include "../third_party/stb_image_resize2.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace batchpress {

// ── Supported extensions ──────────────────────────────────────────────────────

static const std::vector<std::string> SUPPORTED_EXT = {
    ".jpg", ".jpeg", ".png", ".bmp", ".tga", ".hdr", ".pic", ".pnm"
};

static bool is_supported(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    for (const auto& e : SUPPORTED_EXT) if (ext == e) return true;
    return false;
}

// ── collect_images ────────────────────────────────────────────────────────────

std::vector<fs::path> collect_images(const Config& cfg) {
    std::vector<fs::path> result;

    auto add = [&](const fs::directory_entry& e) {
        if (e.is_regular_file() && is_supported(e.path()))
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

// ── SHA-256 (portable — no OpenSSL required) ──────────────────────────────────
// Records the fingerprint of originals before Direct (unsafe) overwrites.

namespace sha256 {

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

inline uint32_t rotr(uint32_t x,uint32_t n){return(x>>n)|(x<<(32-n));}
inline uint32_t ch (uint32_t e,uint32_t f,uint32_t g){return(e&f)^(~e&g);}
inline uint32_t maj(uint32_t a,uint32_t b,uint32_t c){return(a&b)^(a&c)^(b&c);}
inline uint32_t S0(uint32_t a){return rotr(a,2)^rotr(a,13)^rotr(a,22);}
inline uint32_t S1(uint32_t e){return rotr(e,6)^rotr(e,11)^rotr(e,25);}
inline uint32_t s0(uint32_t w){return rotr(w,7)^rotr(w,18)^(w>>3);}
inline uint32_t s1(uint32_t w){return rotr(w,17)^rotr(w,19)^(w>>10);}

std::string hash(const uint8_t* data, size_t len) {
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint64_t bit_len = static_cast<uint64_t>(len)*8;
    size_t padded=((len+8)/64+1)*64;
    std::vector<uint8_t> msg(padded,0);
    std::memcpy(msg.data(),data,len);
    msg[len]=0x80;
    for(int i=7;i>=0;--i) msg[padded-8+(7-i)]=static_cast<uint8_t>(bit_len>>(i*8));
    for(size_t i=0;i<padded;i+=64){
        uint32_t w[64];
        for(int j=0;j<16;++j)
            w[j]=(msg[i+j*4]<<24)|(msg[i+j*4+1]<<16)|(msg[i+j*4+2]<<8)|msg[i+j*4+3];
        for(int j=16;j<64;++j) w[j]=s1(w[j-2])+w[j-7]+s0(w[j-15])+w[j-16];
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(int j=0;j<64;++j){
            uint32_t t1=hh+S1(e)+ch(e,f,g)+K[j]+w[j];
            uint32_t t2=S0(a)+maj(a,b,c);
            hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    std::ostringstream ss;
    for(int i=0;i<8;++i) ss<<std::hex<<std::setw(8)<<std::setfill('0')<<h[i];
    return ss.str();
}

} // namespace sha256

// ── stb write-to-memory callback ──────────────────────────────────────────────

static void stb_write_cb(void* ctx, void* data, int size) {
    auto* buf = static_cast<std::vector<uint8_t>*>(ctx);
    auto* ptr = static_cast<uint8_t*>(data);
    buf->insert(buf->end(), ptr, ptr + size);
}

// ── encode_to_memory ──────────────────────────────────────────────────────────

static std::vector<uint8_t> encode_to_memory(
    const uint8_t* pixels, int w, int h, int ch,
    const std::string& ext, int quality)
{
    std::vector<uint8_t> buf;
    buf.reserve(static_cast<size_t>(w * h * ch / 4));
    int ok = 0;
    if      (ext==".jpg"||ext==".jpeg") ok=stbi_write_jpg_to_func(stb_write_cb,&buf,w,h,ch,pixels,quality);
    else if (ext==".png")               ok=stbi_write_png_to_func(stb_write_cb,&buf,w,h,ch,pixels,w*ch);
    else if (ext==".bmp")               ok=stbi_write_bmp_to_func(stb_write_cb,&buf,w,h,ch,pixels);
    else                                ok=stbi_write_png_to_func(stb_write_cb,&buf,w,h,ch,pixels,w*ch);
    if (!ok || buf.empty())
        throw std::runtime_error("encode_to_memory failed for ext: " + ext);
    return buf;
}

// ── write_safe ────────────────────────────────────────────────────────────────

static void write_safe(const std::vector<uint8_t>& encoded, const fs::path& out) {
    fs::path tmp = out; tmp += ".batchpress.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("Cannot open tmp: " + tmp.string());
        f.write(reinterpret_cast<const char*>(encoded.data()),
                static_cast<std::streamsize>(encoded.size()));
        if (!f) throw std::runtime_error("Write failed: " + tmp.string());
    }
    fs::rename(tmp, out);
}

// ── write_direct ──────────────────────────────────────────────────────────────

static void write_direct(const std::vector<uint8_t>& encoded, const fs::path& out) {
    std::ofstream f(out, std::ios::binary|std::ios::in|std::ios::out|std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open for direct write: " + out.string());
    f.write(reinterpret_cast<const char*>(encoded.data()),
            static_cast<std::streamsize>(encoded.size()));
    if (!f) throw std::runtime_error("Direct write failed: " + out.string());
    f.close();
    fs::resize_file(out, encoded.size());
}

// ── process_image ─────────────────────────────────────────────────────────────

TaskResult process_image(const fs::path& input_path, const Config& cfg) {
    using Clock = std::chrono::high_resolution_clock;
    auto t0 = Clock::now();

    TaskResult result;
    result.input_path  = input_path;
    result.dry_run     = cfg.dry_run;
    result.input_bytes = fs::exists(input_path) ? fs::file_size(input_path) : 0;

    try {
        // ── 1. Output path ────────────────────────────────────────────────
        fs::path out_path = cfg.inplace()
            ? input_path
            : cfg.output_dir / fs::relative(input_path, cfg.input_dir);

        if (cfg.format != ImageFormat::Same)
            out_path.replace_extension(format_extension(cfg.format));

        result.output_path = out_path;

        // ── 2. Skip check (copy mode only) ────────────────────────────────
        if (!cfg.inplace() && !cfg.dry_run
            && cfg.skip_existing && fs::exists(out_path))
        {
            result.success      = true;
            result.skipped      = true;
            result.output_bytes = fs::file_size(out_path);
            result.elapsed_ms   = std::chrono::duration<double,std::milli>(
                                      Clock::now()-t0).count();
            return result;
        }

        // ── 2b. Duplicate detection via hash cache ────────────────────────
        // Only check duplicates if skip_existing didn't trigger
        // Also skip if in-place (dedup only makes sense for copy mode)
        std::string input_sha256;
        if (cfg.dedup_enabled && cfg.hash_cache && !cfg.inplace() && !cfg.dry_run) {
            // Calculate hash of input file
            {
                std::ifstream raw(input_path, std::ios::binary);
                std::vector<uint8_t> raw_bytes(
                    (std::istreambuf_iterator<char>(raw)),
                     std::istreambuf_iterator<char>());
                input_sha256 = sha256::hash(raw_bytes.data(), raw_bytes.size());
            }

            // Check cache for duplicate
            if (const auto* cached = cfg.hash_cache->get(input_sha256)) {
                // Found duplicate! Copy cached output
                if (!cfg.dry_run) {
                    if (!cfg.inplace()) {
                        fs::create_directories(out_path.parent_path());
                    }
                    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
                    f.write(reinterpret_cast<const char*>(cached->data()),
                            static_cast<std::streamsize>(cached->size()));
                    if (!f) throw std::runtime_error("Cannot write cached output: " + out_path.string());
                }
                
                result.success      = true;
                result.skipped      = true;
                result.is_duplicate = true;  // Mark as duplicate (not skip_existing)
                result.output_bytes = cached->size();
                result.elapsed_ms   = std::chrono::duration<double,std::milli>(
                                          Clock::now()-t0).count();
                return result;
            }
        }

        // ── 3. Load into RAM ──────────────────────────────────────────────
        int src_w=0, src_h=0, channels=0;
        unsigned char* pixels = stbi_load(
            input_path.string().c_str(), &src_w, &src_h, &channels, 0);
        if (!pixels)
            throw std::runtime_error(std::string("stbi_load: ")
                                     + stbi_failure_reason());

        struct PixelGuard {
            unsigned char* p;
            explicit PixelGuard(unsigned char* px):p(px){}
            ~PixelGuard(){if(p)stbi_image_free(p);}
            PixelGuard(const PixelGuard&)=delete;
        } guard(pixels);

        // ── 4. Resize in RAM ──────────────────────────────────────────────
        unsigned char* write_pixels = pixels;
        int dst_w=src_w, dst_h=src_h;
        std::vector<unsigned char> resized_buf;

        if (cfg.resize.active()) {
            auto [nw,nh] = cfg.resize.compute(
                static_cast<uint32_t>(src_w),
                static_cast<uint32_t>(src_h));
            dst_w=static_cast<int>(nw);
            dst_h=static_cast<int>(nh);
            resized_buf.resize(static_cast<size_t>(dst_w*dst_h*channels));
            if (!stbir_resize_uint8_linear(
                    pixels, src_w, src_h, 0,
                    resized_buf.data(), dst_w, dst_h, 0,
                    static_cast<stbir_pixel_layout>(channels)))
                throw std::runtime_error("stbir_resize failed");
            write_pixels = resized_buf.data();
        }

        // ── 5. Encode to RAM ──────────────────────────────────────────────
        std::string ext = out_path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        std::vector<uint8_t> encoded = encode_to_memory(
            write_pixels, dst_w, dst_h, channels, ext, cfg.quality);

        result.output_bytes = encoded.size();

        // ── 6. Dry-run: stop here ─────────────────────────────────────────
        if (cfg.dry_run) {
            result.success    = true;
            result.elapsed_ms = std::chrono::duration<double,std::milli>(
                                    Clock::now()-t0).count();
            return result;
        }

        // ── 7. Adaptive write strategy ────────────────────────────────────
        constexpr uint64_t MARGIN = 512ULL * 1024; // 512 KB safety margin

        if (!cfg.inplace()) {
            fs::create_directories(out_path.parent_path());
            write_safe(encoded, out_path);
            result.write_mode = WriteMode::Safe;
        } else {
            // In-place: write to output path (may have different extension)
            uint64_t free_bytes = disk_free_bytes(input_path.parent_path());

            // Check for test override: BATCHPRESS_FORCE_DIRECT=1
            bool force_direct = (std::getenv("BATCHPRESS_FORCE_DIRECT") != nullptr);

            if (force_direct || free_bytes >= encoded.size() + MARGIN) {
                if (force_direct) {
                    // Hash original before overwriting (for recovery log)
                    std::ifstream raw(input_path, std::ios::binary);
                    std::vector<uint8_t> raw_bytes(
                        (std::istreambuf_iterator<char>(raw)),
                         std::istreambuf_iterator<char>());
                    result.original_sha256 = sha256::hash(
                        raw_bytes.data(), raw_bytes.size());
                    write_direct(encoded, out_path);
                    result.write_mode = WriteMode::Direct;
                } else {
                    write_safe(encoded, out_path);
                    result.write_mode = WriteMode::Safe;
                }
            } else {
                // Hash original before overwriting (for recovery log)
                {
                    std::ifstream raw(input_path, std::ios::binary);
                    std::vector<uint8_t> raw_bytes(
                        (std::istreambuf_iterator<char>(raw)),
                         std::istreambuf_iterator<char>());
                    result.original_sha256 = sha256::hash(
                        raw_bytes.data(), raw_bytes.size());
                }
                write_direct(encoded, out_path);
                result.write_mode = WriteMode::Direct;
            }

            // If format changed, remove the original file (e.g., .jpg → .webp)
            if (out_path != input_path && fs::exists(input_path)) {
                fs::remove(input_path);
            }
        }

        result.success      = true;
        result.output_bytes = fs::file_size(out_path);

        // ── 8. Save to cache for duplicate detection ────────────────────────
        if (cfg.dedup_enabled && cfg.hash_cache && !input_sha256.empty()) {
            cfg.hash_cache->put(input_sha256, encoded);
        }

    } catch (const std::exception& ex) {
        result.success   = false;
        result.error_msg = ex.what();
    }

    result.elapsed_ms = std::chrono::duration<double,std::milli>(
                            Clock::now()-t0).count();
    return result;
}

// ── run_batch ─────────────────────────────────────────────────────────────────

BatchReport run_batch(const Config& cfg) {
    using Clock = std::chrono::steady_clock;

    // Initialize hash cache if dedup is enabled
    Config cfg_with_cache = cfg;
    if (cfg.dedup_enabled && !cfg.hash_cache) {
        cfg_with_cache.hash_cache = std::make_shared<HashCache>();
    }

    auto images = collect_images(cfg_with_cache);
    if (images.empty()) return {};

    if (!cfg.inplace() && !cfg.dry_run)
        fs::create_directories(cfg.output_dir);

    BatchReport report;
    report.total   = static_cast<uint32_t>(images.size());
    report.dry_run = cfg.dry_run;

    std::mutex      report_mu;
    std::atomic<uint32_t> done{0};

    size_t threads = cfg.num_threads > 0
        ? cfg.num_threads
        : std::thread::hardware_concurrency();

    ThreadPool pool(threads);
    auto t0 = Clock::now();

    std::vector<std::future<TaskResult>> futures;
    futures.reserve(images.size());
    for (const auto& img : images)
        futures.push_back(pool.submit(process_image, img, std::cref(cfg_with_cache)));

    for (auto& fut : futures) {
        TaskResult res = fut.get();
        uint32_t n = done.fetch_add(1, std::memory_order_relaxed) + 1;

        // ── Aggregate report (thread-safe) ────────────────────────────────
        {
            std::lock_guard lock(report_mu);
            if (res.skipped) {
                if (res.is_duplicate) {
                    ++report.duplicates_found;
                } else {
                    ++report.skipped;  // skip_existing
                }
            } else if (res.success) {
                ++report.succeeded;
                report.input_bytes_total  += res.input_bytes;
                report.output_bytes_total += res.output_bytes;
                if (res.write_mode == WriteMode::Safe)   ++report.written_safe;
                if (res.write_mode == WriteMode::Direct) ++report.written_direct;
            } else {
                ++report.failed;
            }
        }

        // ── Notify UI via callback ────────────────────────────────────────
        // The core never writes to stdout/stderr here.
        // The UI layer decides what to do with this result.
        if (cfg.on_progress)
            cfg.on_progress(res, n, report.total);
    }

    report.elapsed_sec = std::chrono::duration<double>(Clock::now()-t0).count();
    return report;
}

} // namespace batchpress
