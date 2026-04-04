// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.marco@gmail.com>
 *
 * This file is part of batchpress — Android JNI bridge.
 *
 * Exposes ALL core library functionality to Java/Kotlin:
 *   - runBatch()          — traditional image batch processing
 *   - runVideoBatch()     — traditional video batch processing
 *   - scanFiles()         — per-file scan with projected savings
 *   - processFiles()      — selective image processing
 *   - processVideoFiles() — selective video processing
 *   - runScan()           — legacy per-directory image scan
 *   - runVideoScan()      — legacy per-directory video scan
 *   - diskFreeBytes()     — available disk space
 *
 * The core library is completely unmodified — this file is the only
 * Android-specific code in the entire project.
 */

#include <jni.h>
#include <android/log.h>
#include <batchpress/processor.hpp>
#include <batchpress/scanner.hpp>
#include <batchpress/video_processor.hpp>
#include <string>
#include <vector>
#include <shared_mutex>

#define LOG_TAG  "batchpress"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── Helper macros ─────────────────────────────────────────────────────────────

#define JSTR(env, js) ([](JNIEnv* e, jstring s) { \
    if (!s) return std::string(""); \
    const char* c = e->GetStringUTFChars(s, nullptr); \
    std::string r(c); e->ReleaseStringUTFChars(s, c); return r; \
})(env, js)

// ── Global JavaVM reference ──────────────────────────────────────────────────

static JavaVM* g_jvm = nullptr;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

extern "C" {

// ══════════════════════════════════════════════════════════════════════════════
//  IMAGE BATCH — runBatch()
// ══════════════════════════════════════════════════════════════════════════════

JNIEXPORT jobject JNICALL
Java_com_batchpress_BatchPress_runBatch(
    JNIEnv*  env, jclass,
    jstring  j_input_dir,
    jstring  j_output_dir,
    jstring  j_resize,
    jstring  j_format,
    jint     j_quality,
    jint     j_threads,
    jboolean j_dry_run,
    jboolean j_dedup,
    jobject  j_listener)
{
    batchpress::Config cfg;
    cfg.input_dir   = JSTR(env, j_input_dir);
    cfg.output_dir  = JSTR(env, j_output_dir);
    cfg.quality     = j_quality;
    cfg.num_threads = static_cast<size_t>(j_threads);
    cfg.dry_run     = j_dry_run;
    cfg.dedup_enabled = j_dedup;
    cfg.resize      = batchpress::parse_resize(JSTR(env, j_resize));
    cfg.format      = batchpress::parse_format(JSTR(env, j_format));

    if (j_listener) {
        jobject g_listener = env->NewGlobalRef(j_listener);
        jclass  lc = env->GetObjectClass(j_listener);
        jmethodID mid = env->GetMethodID(lc, "onProgress",
            "(Ljava/lang/String;ZIZJJZ)V");

        cfg.on_progress = [g_listener, mid](
            const batchpress::TaskResult& res, uint64_t done, uint64_t total) {
            JNIEnv* cb_env = nullptr;
            bool attached = false;
            if (g_jvm->GetEnv(reinterpret_cast<void**>(&cb_env), JNI_VERSION_1_6) == JNI_EDETACHED) {
                g_jvm->AttachCurrentThread(&cb_env, nullptr);
                attached = true;
            }
            if (cb_env && mid) {
                jstring p = cb_env->NewStringUTF(
                    res.input_path.filename().string().c_str());
                cb_env->CallVoidMethod(g_listener, mid, p,
                    static_cast<jboolean>(res.success),
                    static_cast<jboolean>(res.skipped),
                    static_cast<jint>(done),
                    static_cast<jint>(total),
                    static_cast<jlong>(res.input_bytes),
                    static_cast<jlong>(res.output_bytes),
                    static_cast<jboolean>(res.dry_run));
                cb_env->DeleteLocalRef(p);
            }
            if (attached) g_jvm->DetachCurrentThread();
        };
    }

    batchpress::BatchReport report;
    try {
        report = batchpress::run_batch(cfg);
    } catch (const std::exception& ex) {
        LOGE("run_batch exception: %s", ex.what());
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), ex.what());
        return nullptr;
    }

    jclass rc = env->FindClass("com/batchpress/BatchResult");
    jmethodID ctor = env->GetMethodID(rc, "<init>", "(IIIIIIJJDZ)V");
    return env->NewObject(rc, ctor,
        static_cast<jint>(report.total),
        static_cast<jint>(report.succeeded),
        static_cast<jint>(report.skipped),
        static_cast<jint>(report.failed),
        static_cast<jint>(report.written_safe),
        static_cast<jint>(report.written_direct),
        static_cast<jlong>(report.input_bytes_total),
        static_cast<jlong>(report.output_bytes_total),
        static_cast<jdouble>(report.elapsed_sec),
        static_cast<jboolean>(report.dry_run));
}

#ifdef BATCHPRESS_HAS_VIDEO
// ══════════════════════════════════════════════════════════════════════════════
//  VIDEO BATCH — runVideoBatch()
// ══════════════════════════════════════════════════════════════════════════════

JNIEXPORT jobject JNICALL
Java_com_batchpress_BatchPress_runVideoBatch(
    JNIEnv*  env, jclass,
    jstring  j_input_dir,
    jstring  j_output_dir,
    jstring  j_vcodec,
    jint     j_crf,
    jstring  j_max_res,
    jint     j_audio_bps,
    jint     j_threads,
    jboolean j_dry_run,
    jboolean j_dedup,
    jobject  j_listener)
{
    batchpress::VideoConfig cfg;
    cfg.input_dir   = JSTR(env, j_input_dir);
    cfg.output_dir  = JSTR(env, j_output_dir);
    cfg.num_threads = static_cast<size_t>(j_threads);
    cfg.dry_run     = j_dry_run;
    cfg.dedup_enabled = j_dedup;

    std::string vc = JSTR(env, j_vcodec);
    if      (vc == "h265") cfg.video_codec = batchpress::VideoCodec::H265;
    else if (vc == "h264") cfg.video_codec = batchpress::VideoCodec::H264;
    else if (vc == "vp9")  cfg.video_codec = batchpress::VideoCodec::VP9;
    else                   cfg.video_codec = batchpress::VideoCodec::Auto;

    if (j_crf >= 0) cfg.crf = j_crf;

    std::string mr = JSTR(env, j_max_res);
    if      (mr == "1080p")    cfg.resolution = batchpress::ResolutionCap::Cap1080p;
    else if (mr == "4k")       cfg.resolution = batchpress::ResolutionCap::Cap4K;
    else if (mr == "original") cfg.resolution = batchpress::ResolutionCap::Original;
    else                       cfg.resolution = batchpress::ResolutionCap::Cap1080p;

    if (j_audio_bps >= 0) cfg.audio_bitrate_kbps = j_audio_bps;

    if (j_listener) {
        jobject g_listener = env->NewGlobalRef(j_listener);
        jclass  lc = env->GetObjectClass(j_listener);
        jmethodID mid = env->GetMethodID(lc, "onVideoProgress",
            "(Ljava/lang/String;JJII)V");

        cfg.on_progress = [g_listener, mid](
            const batchpress::fs::path& path,
            uint64_t fdone, uint64_t ftotal,
            uint32_t fdn, uint32_t ftt) {
            JNIEnv* cb_env = nullptr;
            bool attached = false;
            if (g_jvm->GetEnv(reinterpret_cast<void**>(&cb_env), JNI_VERSION_1_6) == JNI_EDETACHED) {
                g_jvm->AttachCurrentThread(&cb_env, nullptr);
                attached = true;
            }
            if (cb_env && mid) {
                jstring p = cb_env->NewStringUTF(path.filename().string().c_str());
                cb_env->CallVoidMethod(g_listener, mid, p,
                    static_cast<jlong>(fdone), static_cast<jlong>(ftotal),
                    static_cast<jint>(fdn), static_cast<jint>(ftt));
                cb_env->DeleteLocalRef(p);
            }
            if (attached) g_jvm->DetachCurrentThread();
        };
    }

    batchpress::VideoBatchReport report;
    try {
        report = batchpress::run_video_batch(cfg);
    } catch (const std::exception& ex) {
        LOGE("run_video_batch exception: %s", ex.what());
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), ex.what());
        return nullptr;
    }

    jclass rc = env->FindClass("com/batchpress/VideoBatchResult");
    jmethodID ctor = env->GetMethodID(rc, "<init>", "(IIIIJJIIIDZ)V");
    return env->NewObject(rc, ctor,
        static_cast<jint>(report.total),
        static_cast<jint>(report.succeeded),
        static_cast<jint>(report.skipped),
        static_cast<jint>(report.failed),
        static_cast<jlong>(report.input_bytes_total),
        static_cast<jlong>(report.output_bytes_total),
        static_cast<jint>(report.used_h265),
        static_cast<jint>(report.used_h264),
        static_cast<jint>(report.used_vp9),
        static_cast<jdouble>(report.elapsed_sec),
        static_cast<jboolean>(report.dry_run));
}

// ══════════════════════════════════════════════════════════════════════════════
//  FILE SCAN — scanFiles()
// ══════════════════════════════════════════════════════════════════════════════

JNIEXPORT jobject JNICALL
Java_com_batchpress_BatchPress_scanFiles(
    JNIEnv*  env, jclass,
    jstring  j_root_dir,
    jboolean j_recursive,
    jint     j_samples,
    jint     j_threads,
    jobject  j_listener)
{
    batchpress::ScanConfig cfg;
    cfg.root_dir       = JSTR(env, j_root_dir);
    cfg.recursive      = j_recursive;
    cfg.samples_per_dir = static_cast<uint32_t>(j_samples);
    cfg.num_threads    = static_cast<size_t>(j_threads);

    if (j_listener) {
        jobject g_listener = env->NewGlobalRef(j_listener);
        jclass  lc = env->GetObjectClass(j_listener);
        jmethodID mid = env->GetMethodID(lc, "onScanProgress",
            "(Ljava/lang/String;II)V");

        cfg.on_progress = [g_listener, mid](
            const std::string& name, uint32_t done, uint32_t total) {
            JNIEnv* cb_env = nullptr;
            bool attached = false;
            if (g_jvm->GetEnv(reinterpret_cast<void**>(&cb_env), JNI_VERSION_1_6) == JNI_EDETACHED) {
                g_jvm->AttachCurrentThread(&cb_env, nullptr);
                attached = true;
            }
            if (cb_env && mid) {
                jstring p = cb_env->NewStringUTF(name.c_str());
                cb_env->CallVoidMethod(g_listener, mid, p,
                    static_cast<jint>(done), static_cast<jint>(total));
                cb_env->DeleteLocalRef(p);
            }
            if (attached) g_jvm->DetachCurrentThread();
        };
    }

    batchpress::FileScanReport report;
    try {
        report = batchpress::scan_files(cfg);
    } catch (const std::exception& ex) {
        LOGE("scan_files exception: %s", ex.what());
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), ex.what());
        return nullptr;
    }

    // Build FileItem[]
    jclass fi_cls = env->FindClass("com/batchpress/FileItem");
    jmethodID fi_ctor = env->GetMethodID(fi_cls, "<init>",
        "(ZLjava/lang/String;Ljava/lang/String;JJJIIJJ"
        "Ljava/lang/String;Ljava/lang/String;"
        "DLjava/lang/String;Ljava/lang/String;Ljava/lang/String;"
        "Ljava/lang/String;IIIIVI)V");

    jobjectArray arr = env->NewObjectArray(
        static_cast<jsize>(report.files.size()), fi_cls, nullptr);

    for (size_t i = 0; i < report.files.size(); ++i) {
        const auto& f = report.files[i];
        bool is_video = (f.type == batchpress::FileItem::Type::Video);

        // Get quality info
        batchpress::QualityEstimate q;
        if (is_video)
            q = f.video_info().quality;
        else
            q = f.image_info().quality;
        const char* qlabel = batchpress::quality_label(q);
        int qstars = batchpress::quality_stars(q);

        // Get projected dimensions
        uint32_t pw = 0, ph = 0;
        if (is_video) {
            pw = f.video_info().projected_width;
            ph = f.video_info().projected_height;
        } else {
            pw = f.image_info().projected_width;
            ph = f.image_info().projected_height;
        }

        jstring j_path      = env->NewStringUTF(f.path.string().c_str());
        jstring j_fname     = env->NewStringUTF(f.filename.c_str());
        jstring j_fmt       = env->NewStringUTF(
            is_video ? f.video_info().container.c_str() : f.image_info().format.c_str());
        jstring j_codec     = env->NewStringUTF(
            is_video ? f.video_info().suggested_codec.c_str()
                     : f.image_info().suggested_codec.c_str());
        jstring j_vcodec    = is_video ? env->NewStringUTF(f.video_info().video_codec.c_str()) : nullptr;
        jstring j_acodec    = is_video ? env->NewStringUTF(f.video_info().audio_codec.c_str()) : nullptr;
        jstring j_qlabel    = env->NewStringUTF(qlabel);

        jobject item = env->NewObject(fi_cls, fi_ctor,
            static_cast<jboolean>(is_video),
            j_path, j_fname,
            static_cast<jlong>(0),   // creation_time
            static_cast<jlong>(0),   // last_access
            static_cast<jlong>(0),   // last_modified
            static_cast<jint>(f.width),
            static_cast<jint>(f.height),
            static_cast<jlong>(f.file_size),
            static_cast<jlong>(f.projected_size),
            static_cast<jdouble>(f.savings_pct),
            j_fmt, j_codec,
            static_cast<jdouble>(is_video ? f.video_info().duration_sec : 0.0),
            j_vcodec, j_acodec,
            j_path,  // display hint
            j_qlabel, static_cast<jint>(qstars),
            static_cast<jint>(pw), static_cast<jint>(ph));

        env->SetObjectArrayElement(arr, static_cast<jsize>(i), item);

        env->DeleteLocalRef(j_path);
        env->DeleteLocalRef(j_fname);
        env->DeleteLocalRef(j_fmt);
        env->DeleteLocalRef(j_codec);
        if (j_vcodec) env->DeleteLocalRef(j_vcodec);
        if (j_acodec) env->DeleteLocalRef(j_acodec);
        env->DeleteLocalRef(j_qlabel);
        env->DeleteLocalRef(item);
    }

    jclass report_cls = env->FindClass("com/batchpress/FileScanReport");
    jmethodID r_ctor = env->GetMethodID(report_cls, "<init>",
        "([Ljava/lang/Object;DDDII)V");

    return env->NewObject(report_cls, r_ctor, arr,
        static_cast<jdouble>(report.elapsed_sec),
        static_cast<jdouble>(report.overall_savings_pct()),
        static_cast<jdouble>(report.total_size()),
        static_cast<jint>(report.image_count()),
        static_cast<jint>(report.video_count()));
}

// ══════════════════════════════════════════════════════════════════════════════
//  SELECTIVE IMAGE PROCESSING — processFiles()
// ══════════════════════════════════════════════════════════════════════════════

JNIEXPORT jobject JNICALL
Java_com_batchpress_BatchPress_processFiles(
    JNIEnv*  env, jclass,
    jobjectArray j_files,
    jstring  j_input_dir,
    jstring  j_output_dir,
    jstring  j_resize,
    jstring  j_format,
    jint     j_quality,
    jint     j_threads,
    jboolean j_dry_run,
    jboolean j_dedup,
    jobject  j_listener)
{
    batchpress::Config cfg;
    cfg.input_dir   = JSTR(env, j_input_dir);
    cfg.output_dir  = JSTR(env, j_output_dir);
    cfg.quality     = j_quality;
    cfg.num_threads = static_cast<size_t>(j_threads);
    cfg.dry_run     = j_dry_run;
    cfg.dedup_enabled = j_dedup;
    cfg.resize      = batchpress::parse_resize(JSTR(env, j_resize));
    cfg.format      = batchpress::parse_format(JSTR(env, j_format));

    // Extract paths from FileItem[]
    jsize len = env->GetArrayLength(j_files);
    std::vector<batchpress::FileItem> items;
    items.reserve(len);

    jclass fi_cls = env->FindClass("com/batchpress/FileItem");
    jfieldID f_path   = env->GetFieldID(fi_cls, "path", "Ljava/lang/String;");
    jfieldID f_is_vid = env->GetFieldID(fi_cls, "isVideo", "Z");
    jfieldID f_width  = env->GetFieldID(fi_cls, "width", "I");
    jfieldID f_height = env->GetFieldID(fi_cls, "height", "I");
    jfieldID f_fsize  = env->GetFieldID(fi_cls, "fileSize", "J");
    jfieldID f_proj   = env->GetFieldID(fi_cls, "projectedSize", "J");
    jfieldID f_savpct = env->GetFieldID(fi_cls, "savingsPct", "D");
    jfieldID f_fmt    = env->GetFieldID(fi_cls, "format", "Ljava/lang/String;");
    jfieldID f_codec  = env->GetFieldID(fi_cls, "suggestedCodec", "Ljava/lang/String;");
    jfieldID f_dur    = env->GetFieldID(fi_cls, "durationSec", "D");
    jfieldID f_vcodec = env->GetFieldID(fi_cls, "videoCodec", "Ljava/lang/String;");
    jfieldID f_acodec = env->GetFieldID(fi_cls, "audioCodec", "Ljava/lang/String;");
    jfieldID f_qlabel = env->GetFieldID(fi_cls, "qualityLabel", "Ljava/lang/String;");
    jfieldID f_qstars = env->GetFieldID(fi_cls, "qualityStars", "I");

    for (jsize i = 0; i < len; ++i) {
        jobject obj = env->GetObjectArrayElement(j_files, i);
        if (!obj) continue;

        batchpress::FileItem fi;
        jstring js = (jstring) env->GetObjectField(obj, f_path);
        fi.path = JSTR(env, js);
        env->DeleteLocalRef(js);
        fi.filename = fi.path.filename().string();
        fi.type = env->GetBooleanField(obj, f_is_vid)
            ? batchpress::FileItem::Type::Video
            : batchpress::FileItem::Type::Image;
        fi.width  = env->GetIntField(obj, f_width);
        fi.height = env->GetIntField(obj, f_height);
        fi.file_size     = env->GetLongField(obj, f_fsize);
        fi.projected_size = env->GetLongField(obj, f_proj);
        fi.savings_pct   = env->GetDoubleField(obj, f_savpct);

        if (fi.type == batchpress::FileItem::Type::Image) {
            batchpress::ImageFileInfo im;
            js = (jstring) env->GetObjectField(obj, f_fmt);
            im.format = JSTR(env, js); env->DeleteLocalRef(js);
            js = (jstring) env->GetObjectField(obj, f_codec);
            im.suggested_codec = JSTR(env, js); env->DeleteLocalRef(js);
            fi.meta = std::move(im);
        } else {
            batchpress::VideoFileInfo vi;
            vi.duration_sec = env->GetDoubleField(obj, f_dur);
            js = (jstring) env->GetObjectField(obj, f_vcodec);
            vi.video_codec = JSTR(env, js); env->DeleteLocalRef(js);
            js = (jstring) env->GetObjectField(obj, f_acodec);
            vi.audio_codec = JSTR(env, js); env->DeleteLocalRef(js);
            js = (jstring) env->GetObjectField(obj, f_fmt);
            vi.container = JSTR(env, js); env->DeleteLocalRef(js);
            js = (jstring) env->GetObjectField(obj, f_codec);
            vi.suggested_codec = JSTR(env, js); env->DeleteLocalRef(js);
            fi.meta = std::move(vi);
        }

        items.push_back(std::move(fi));
        env->DeleteLocalRef(obj);
    }

    // Filter only images for processFiles
    std::vector<batchpress::FileItem> image_items;
    for (auto& it : items)
        if (it.type == batchpress::FileItem::Type::Image)
            image_items.push_back(std::move(it));

    if (j_listener) {
        jobject g_listener = env->NewGlobalRef(j_listener);
        jclass  lc = env->GetObjectClass(j_listener);
        jmethodID mid = env->GetMethodID(lc, "onProgress",
            "(Ljava/lang/String;ZIZJJZ)V");

        cfg.on_progress = [g_listener, mid](
            const batchpress::TaskResult& res, uint64_t done, uint64_t total) {
            JNIEnv* cb_env = nullptr;
            bool attached = false;
            if (g_jvm->GetEnv(reinterpret_cast<void**>(&cb_env), JNI_VERSION_1_6) == JNI_EDETACHED) {
                g_jvm->AttachCurrentThread(&cb_env, nullptr);
                attached = true;
            }
            if (cb_env && mid) {
                jstring p = cb_env->NewStringUTF(
                    res.input_path.filename().string().c_str());
                cb_env->CallVoidMethod(g_listener, mid, p,
                    static_cast<jboolean>(res.success),
                    static_cast<jboolean>(res.skipped),
                    static_cast<jint>(done),
                    static_cast<jint>(total),
                    static_cast<jlong>(res.input_bytes),
                    static_cast<jlong>(res.output_bytes),
                    static_cast<jboolean>(res.dry_run));
                cb_env->DeleteLocalRef(p);
            }
            if (attached) g_jvm->DetachCurrentThread();
        };
    }

    batchpress::BatchReport report;
    try {
        report = batchpress::process_files(image_items, cfg);
    } catch (const std::exception& ex) {
        LOGE("process_files exception: %s", ex.what());
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), ex.what());
        return nullptr;
    }

    jclass rc = env->FindClass("com/batchpress/BatchResult");
    jmethodID ctor = env->GetMethodID(rc, "<init>", "(IIIIIIJJDZ)V");
    return env->NewObject(rc, ctor,
        static_cast<jint>(report.total),
        static_cast<jint>(report.succeeded),
        static_cast<jint>(report.skipped),
        static_cast<jint>(report.failed),
        static_cast<jint>(report.written_safe),
        static_cast<jint>(report.written_direct),
        static_cast<jlong>(report.input_bytes_total),
        static_cast<jlong>(report.output_bytes_total),
        static_cast<jdouble>(report.elapsed_sec),
        static_cast<jboolean>(report.dry_run));
}

// ══════════════════════════════════════════════════════════════════════════════
//  SELECTIVE VIDEO PROCESSING — processVideoFiles()
// ══════════════════════════════════════════════════════════════════════════════

JNIEXPORT jobject JNICALL
Java_com_batchpress_BatchPress_processVideoFiles(
    JNIEnv*  env, jclass,
    jobjectArray j_files,
    jstring  j_input_dir,
    jstring  j_output_dir,
    jstring  j_vcodec,
    jint     j_crf,
    jstring  j_max_res,
    jint     j_audio_bps,
    jint     j_threads,
    jboolean j_dry_run,
    jboolean j_dedup,
    jobject  j_listener)
{
    batchpress::VideoConfig cfg;
    cfg.input_dir   = JSTR(env, j_input_dir);
    cfg.output_dir  = JSTR(env, j_output_dir);
    cfg.num_threads = static_cast<size_t>(j_threads);
    cfg.dry_run     = j_dry_run;
    cfg.dedup_enabled = j_dedup;

    std::string vc = JSTR(env, j_vcodec);
    if      (vc == "h265") cfg.video_codec = batchpress::VideoCodec::H265;
    else if (vc == "h264") cfg.video_codec = batchpress::VideoCodec::H264;
    else if (vc == "vp9")  cfg.video_codec = batchpress::VideoCodec::VP9;
    else                   cfg.video_codec = batchpress::VideoCodec::Auto;
    if (j_crf >= 0) cfg.crf = j_crf;

    std::string mr = JSTR(env, j_max_res);
    if      (mr == "1080p")    cfg.resolution = batchpress::ResolutionCap::Cap1080p;
    else if (mr == "4k")       cfg.resolution = batchpress::ResolutionCap::Cap4K;
    else if (mr == "original") cfg.resolution = batchpress::ResolutionCap::Original;
    else                       cfg.resolution = batchpress::ResolutionCap::Cap1080p;
    if (j_audio_bps >= 0) cfg.audio_bitrate_kbps = j_audio_bps;

    // Extract video items
    jsize len = env->GetArrayLength(j_files);
    std::vector<batchpress::FileItem> video_items;

    jclass fi_cls = env->FindClass("com/batchpress/FileItem");
    jfieldID f_path   = env->GetFieldID(fi_cls, "path", "Ljava/lang/String;");
    jfieldID f_is_vid = env->GetFieldID(fi_cls, "isVideo", "Z");
    jfieldID f_width  = env->GetFieldID(fi_cls, "width", "I");
    jfieldID f_height = env->GetFieldID(fi_cls, "height", "I");
    jfieldID f_fsize  = env->GetFieldID(fi_cls, "fileSize", "J");
    jfieldID f_proj   = env->GetFieldID(fi_cls, "projectedSize", "J");
    jfieldID f_savpct = env->GetFieldID(fi_cls, "savingsPct", "D");
    jfieldID f_fmt    = env->GetFieldID(fi_cls, "format", "Ljava/lang/String;");
    jfieldID f_codec  = env->GetFieldID(fi_cls, "suggestedCodec", "Ljava/lang/String;");
    jfieldID f_dur    = env->GetFieldID(fi_cls, "durationSec", "D");
    jfieldID f_vcodec = env->GetFieldID(fi_cls, "videoCodec", "Ljava/lang/String;");
    jfieldID f_acodec = env->GetFieldID(fi_cls, "audioCodec", "Ljava/lang/String;");
    jfieldID f_qlabel = env->GetFieldID(fi_cls, "qualityLabel", "Ljava/lang/String;");
    jfieldID f_qstars = env->GetFieldID(fi_cls, "qualityStars", "I");

    for (jsize i = 0; i < len; ++i) {
        jobject obj = env->GetObjectArrayElement(j_files, i);
        if (!obj) continue;

        bool is_vid = env->GetBooleanField(obj, f_is_vid);
        if (!is_vid) { env->DeleteLocalRef(obj); continue; }

        batchpress::FileItem fi;
        fi.type = batchpress::FileItem::Type::Video;

        jstring js = (jstring) env->GetObjectField(obj, f_path);
        fi.path = JSTR(env, js); env->DeleteLocalRef(js);
        fi.filename = fi.path.filename().string();
        fi.width  = env->GetIntField(obj, f_width);
        fi.height = env->GetIntField(obj, f_height);
        fi.file_size     = env->GetLongField(obj, f_fsize);
        fi.projected_size = env->GetLongField(obj, f_proj);
        fi.savings_pct   = env->GetDoubleField(obj, f_savpct);

        batchpress::VideoFileInfo vi;
        vi.duration_sec = env->GetDoubleField(obj, f_dur);
        js = (jstring) env->GetObjectField(obj, f_vcodec);
        vi.video_codec = JSTR(env, js); env->DeleteLocalRef(js);
        js = (jstring) env->GetObjectField(obj, f_acodec);
        vi.audio_codec = JSTR(env, js); env->DeleteLocalRef(js);
        js = (jstring) env->GetObjectField(obj, f_fmt);
        vi.container = JSTR(env, js); env->DeleteLocalRef(js);
        js = (jstring) env->GetObjectField(obj, f_codec);
        vi.suggested_codec = JSTR(env, js); env->DeleteLocalRef(js);
        fi.meta = std::move(vi);

        video_items.push_back(std::move(fi));
        env->DeleteLocalRef(obj);
    }

    if (j_listener) {
        jobject g_listener = env->NewGlobalRef(j_listener);
        jclass  lc = env->GetObjectClass(j_listener);
        jmethodID mid = env->GetMethodID(lc, "onVideoProgress",
            "(Ljava/lang/String;JJII)V");

        cfg.on_progress = [g_listener, mid](
            const batchpress::fs::path& path,
            uint64_t fdone, uint64_t ftotal,
            uint32_t fdn, uint32_t ftt) {
            JNIEnv* cb_env = nullptr;
            bool attached = false;
            if (g_jvm->GetEnv(reinterpret_cast<void**>(&cb_env), JNI_VERSION_1_6) == JNI_EDETACHED) {
                g_jvm->AttachCurrentThread(&cb_env, nullptr);
                attached = true;
            }
            if (cb_env && mid) {
                jstring p = cb_env->NewStringUTF(path.filename().string().c_str());
                cb_env->CallVoidMethod(g_listener, mid, p,
                    static_cast<jlong>(fdone), static_cast<jlong>(ftotal),
                    static_cast<jint>(fdn), static_cast<jint>(ftt));
                cb_env->DeleteLocalRef(p);
            }
            if (attached) g_jvm->DetachCurrentThread();
        };
    }

    batchpress::VideoBatchReport report;
    try {
        report = batchpress::process_video_files(video_items, cfg);
    } catch (const std::exception& ex) {
        LOGE("process_video_files exception: %s", ex.what());
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), ex.what());
        return nullptr;
    }

    jclass rc = env->FindClass("com/batchpress/VideoBatchResult");
    jmethodID ctor = env->GetMethodID(rc, "<init>", "(IIIIJJIIIDZ)V");
    return env->NewObject(rc, ctor,
        static_cast<jint>(report.total),
        static_cast<jint>(report.succeeded),
        static_cast<jint>(report.skipped),
        static_cast<jint>(report.failed),
        static_cast<jlong>(report.input_bytes_total),
        static_cast<jlong>(report.output_bytes_total),
        static_cast<jint>(report.used_h265),
        static_cast<jint>(report.used_h264),
        static_cast<jint>(report.used_vp9),
        static_cast<jdouble>(report.elapsed_sec),
        static_cast<jboolean>(report.dry_run));
}

// ══════════════════════════════════════════════════════════════════════════════
//  LEGACY: per-directory scan (images) — runScan()
// ══════════════════════════════════════════════════════════════════════════════

JNIEXPORT jobject JNICALL
Java_com_batchpress_BatchPress_runScan(
    JNIEnv*  env, jclass,
    jstring  j_root_dir,
    jboolean j_recursive,
    jint     j_samples,
    jint     j_threads,
    jobject  j_listener)
{
    batchpress::ScanConfig cfg;
    cfg.root_dir        = JSTR(env, j_root_dir);
    cfg.recursive       = j_recursive;
    cfg.samples_per_dir = static_cast<uint32_t>(j_samples);
    cfg.num_threads     = static_cast<size_t>(j_threads);

    if (j_listener) {
        jobject g_listener = env->NewGlobalRef(j_listener);
        jclass  lc = env->GetObjectClass(j_listener);
        jmethodID mid = env->GetMethodID(lc, "onScanProgress",
            "(Ljava/lang/String;II)V");

        cfg.on_progress = [g_listener, mid](
            const std::string& name, uint32_t done, uint32_t total) {
            JNIEnv* cb_env = nullptr;
            bool attached = false;
            if (g_jvm->GetEnv(reinterpret_cast<void**>(&cb_env), JNI_VERSION_1_6) == JNI_EDETACHED) {
                g_jvm->AttachCurrentThread(&cb_env, nullptr);
                attached = true;
            }
            if (cb_env && mid) {
                jstring p = cb_env->NewStringUTF(name.c_str());
                cb_env->CallVoidMethod(g_listener, mid, p,
                    static_cast<jint>(done), static_cast<jint>(total));
                cb_env->DeleteLocalRef(p);
            }
            if (attached) g_jvm->DetachCurrentThread();
        };
    }

    batchpress::ScanReport report;
    try {
        report = batchpress::run_scan(cfg);
    } catch (const std::exception& ex) {
        LOGE("run_scan exception: %s", ex.what());
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), ex.what());
        return nullptr;
    }

    // Build simplified scan summary (just the key numbers)
    jclass rc = env->FindClass("com/batchpress/ScanSummary");
    jmethodID ctor = env->GetMethodID(rc, "<init>", "(JJJDLjava/lang/String;)V");

    batchpress::ScanCandidate best = report.global_best_candidate();
    jstring j_best = env->NewStringUTF(best.label().c_str());

    return env->NewObject(rc, ctor,
        static_cast<jlong>(report.total_bytes),
        static_cast<jlong>(report.projected_bytes),
        static_cast<jlong>(report.bytes_saved()),
        static_cast<jdouble>(report.savings_pct()),
        j_best);
}

// ══════════════════════════════════════════════════════════════════════════════
//  LEGACY: per-directory scan (videos) — runVideoScan()
// ══════════════════════════════════════════════════════════════════════════════

JNIEXPORT jobject JNICALL
Java_com_batchpress_BatchPress_runVideoScan(
    JNIEnv*  env, jclass,
    jstring  j_root_dir,
    jboolean j_recursive,
    jint     j_threads,
    jobject  j_listener)
{
    batchpress::VideoScanConfig cfg;
    cfg.root_dir    = JSTR(env, j_root_dir);
    cfg.recursive   = j_recursive;
    cfg.num_threads = static_cast<size_t>(j_threads);

    if (j_listener) {
        jobject g_listener = env->NewGlobalRef(j_listener);
        jclass  lc = env->GetObjectClass(j_listener);
        jmethodID mid = env->GetMethodID(lc, "onScanProgress",
            "(Ljava/lang/String;II)V");

        cfg.on_progress = [g_listener, mid](
            const std::string& name, uint32_t done, uint32_t total) {
            JNIEnv* cb_env = nullptr;
            bool attached = false;
            if (g_jvm->GetEnv(reinterpret_cast<void**>(&cb_env), JNI_VERSION_1_6) == JNI_EDETACHED) {
                g_jvm->AttachCurrentThread(&cb_env, nullptr);
                attached = true;
            }
            if (cb_env && mid) {
                jstring p = cb_env->NewStringUTF(name.c_str());
                cb_env->CallVoidMethod(g_listener, mid, p,
                    static_cast<jint>(done), static_cast<jint>(total));
                cb_env->DeleteLocalRef(p);
            }
            if (attached) g_jvm->DetachCurrentThread();
        };
    }

    batchpress::VideoScanReport report;
    try {
        report = batchpress::run_video_scan(cfg);
    } catch (const std::exception& ex) {
        LOGE("run_video_scan exception: %s", ex.what());
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), ex.what());
        return nullptr;
    }

    jclass rc = env->FindClass("com/batchpress/VideoScanSummary");
    jmethodID ctor = env->GetMethodID(rc, "<init>", "(JJJDLjava/lang/String;I)V");

    std::string codec_str;
    auto caps = report.caps;
    auto bv = caps.best_video();
    if      (bv == batchpress::VideoCodec::H265) codec_str = "H.265";
    else if (bv == batchpress::VideoCodec::H264) codec_str = "H.264";
    else if (bv == batchpress::VideoCodec::VP9)  codec_str = "VP9";
    else codec_str = "auto";

    int crf = (bv == batchpress::VideoCodec::H265) ? 28
              : (bv == batchpress::VideoCodec::H264) ? 26 : 33;

    jstring j_codec = env->NewStringUTF(codec_str.c_str());

    return env->NewObject(rc, ctor,
        static_cast<jlong>(report.total_bytes),
        static_cast<jlong>(report.projected_bytes),
        static_cast<jlong>(report.bytes_saved()),
        static_cast<jdouble>(report.savings_pct()),
        j_codec, static_cast<jint>(crf));
}

// ══════════════════════════════════════════════════════════════════════════════
//  VIDEO FILE SCAN — scanVideoFiles()
// ══════════════════════════════════════════════════════════════════════════════

JNIEXPORT jobject JNICALL
Java_com_batchpress_BatchPress_scanVideoFiles(
    JNIEnv*  env, jclass,
    jstring  j_root_dir,
    jboolean j_recursive,
    jstring  j_vcodec,
    jint     j_crf,
    jstring  j_max_res,
    jint     j_audio_bps,
    jint     j_threads,
    jobject  j_listener)
{
    // Determine resolution cap
    batchpress::ResolutionCap res_cap = batchpress::ResolutionCap::Cap1080p;
    std::string mr = JSTR(env, j_max_res);
    if      (mr == "1080p")    res_cap = batchpress::ResolutionCap::Cap1080p;
    else if (mr == "4k")       res_cap = batchpress::ResolutionCap::Cap4K;
    else if (mr == "original") res_cap = batchpress::ResolutionCap::Original;

    // Collect video paths
    std::vector<batchpress::fs::path> video_paths;
    auto add_video = [&](const batchpress::fs::directory_entry& e) {
        static const std::vector<std::string> video_exts = {
            ".mp4", ".mov", ".mkv", ".avi", ".webm", ".wmv", ".flv",
            ".m4v", ".3gp", ".ts", ".mts", ".m2ts"
        };
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        for (const auto& ve : video_exts)
            if (ext == ve && e.is_regular_file()) { video_paths.push_back(e.path()); return; }
    };

    std::string root = JSTR(env, j_root_dir);
    if (j_recursive) {
        for (const auto& e : batchpress::fs::recursive_directory_iterator(
                batchpress::fs::path(root),
                batchpress::fs::directory_options::skip_permission_denied))
            add_video(e);
    } else {
        for (const auto& e : batchpress::fs::directory_iterator(root))
            add_video(e);
    }

    // Read metadata for each video
    std::vector<batchpress::FileItem> items;
    items.reserve(video_paths.size());

    auto caps = batchpress::probe_codec_caps();
    batchpress::VideoCodec vc = (JSTR(env, j_vcodec) == "h265") ? batchpress::VideoCodec::H265
                                : (JSTR(env, j_vcodec) == "h264") ? batchpress::VideoCodec::H264
                                : (JSTR(env, j_vcodec) == "vp9")   ? batchpress::VideoCodec::VP9
                                : caps.best_video();

    double crf_factor = (vc == batchpress::VideoCodec::H265) ? 0.40
                      : (vc == batchpress::VideoCodec::VP9)  ? 0.45
                      : 0.55;

    int crf = (j_crf >= 0) ? j_crf : (vc == batchpress::VideoCodec::H265) ? 28
              : (vc == batchpress::VideoCodec::H264) ? 26 : 33;

    std::string codec_str;
    if      (vc == batchpress::VideoCodec::H265) codec_str = "H.265";
    else if (vc == batchpress::VideoCodec::H264) codec_str = "H.264";
    else if (vc == batchpress::VideoCodec::VP9)  codec_str = "VP9";
    else codec_str = "auto";

    uint32_t total = static_cast<uint32_t>(video_paths.size());
    uint32_t done = 0;

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

            // Timestamps
            try {
                fi.last_modified = batchpress::fs::last_write_time(vp);
                fi.creation_time = fi.last_modified;
                fi.last_access = fi.last_modified;
            } catch (...) {}

            // Projected resolution
            uint32_t proj_w = static_cast<uint32_t>(vm.width);
            uint32_t proj_h = static_cast<uint32_t>(vm.height);
            if (res_cap == batchpress::ResolutionCap::Cap1080p && vm.height > 1080) {
                double scale = 1080.0 / vm.height;
                proj_w = static_cast<uint32_t>(vm.width * scale + 0.5);
                proj_h = 1080;
            } else if (res_cap == batchpress::ResolutionCap::Cap4K && vm.height > 2160) {
                double scale = 2160.0 / vm.height;
                proj_w = static_cast<uint32_t>(vm.width * scale + 0.5);
                proj_h = 2160;
            }

            // Projected size and quality
            double crf_f = crf_factor;
            uint64_t proj_size = static_cast<uint64_t>(vm.file_bytes * crf_f);
            double savings_pct = 100.0 * (1.0 - crf_f);

            batchpress::QualityEstimate q = batchpress::QualityEstimate::High;
            if (savings_pct > 70.0) q = batchpress::QualityEstimate::Low;
            else if (savings_pct > 40.0) q = batchpress::QualityEstimate::Medium;

            batchpress::VideoFileInfo vi;
            vi.duration_sec = vm.duration_sec;
            vi.video_codec = vm.video_codec_name;
            vi.audio_codec = vm.audio_codec_name;
            vi.container = vm.container_name;
            vi.suggested_codec = codec_str + " CRF" + std::to_string(crf);
            vi.quality = q;
            vi.projected_width = proj_w;
            vi.projected_height = proj_h;

            fi.projected_size = proj_size;
            fi.savings_pct = savings_pct;
            fi.meta = std::move(vi);
            items.push_back(std::move(fi));

            // Progress callback
            if (j_listener) {
                ++done;
                jobject g_listener = env->NewGlobalRef(j_listener);
                jclass lc = env->GetObjectClass(j_listener);
                jmethodID mid = env->GetMethodID(lc, "onScanProgress", "(Ljava/lang/String;II)V");
                if (mid) {
                    JNIEnv* cb_env = nullptr;
                    bool attached = false;
                    if (g_jvm->GetEnv(reinterpret_cast<void**>(&cb_env), JNI_VERSION_1_6) == JNI_EDETACHED) {
                        g_jvm->AttachCurrentThread(&cb_env, nullptr);
                        attached = true;
                    }
                    if (cb_env) {
                        jstring p = cb_env->NewStringUTF(vp.filename().string().c_str());
                        cb_env->CallVoidMethod(g_listener, mid, p, static_cast<jint>(done), static_cast<jint>(total));
                        cb_env->DeleteLocalRef(p);
                    }
                    if (attached) g_jvm->DetachCurrentThread();
                }
                env->DeleteGlobalRef(g_listener);
            }
        } catch (...) {
            // Skip unreadable videos
        }
    }

    // Build FileScanReport
    jclass fi_cls = env->FindClass("com/batchpress/FileItem");
    jmethodID fi_ctor = env->GetMethodID(fi_cls, "<init>",
        "(ZLjava/lang/String;Ljava/lang/String;JJJIIJJ"
        "Ljava/lang/String;Ljava/lang/String;"
        "DLjava/lang/String;Ljava/lang/String;Ljava/lang/String;"
        "Ljava/lang/String;IIIIVI)V");

    jobjectArray arr = env->NewObjectArray(static_cast<jsize>(items.size()), fi_cls, nullptr);

    for (size_t i = 0; i < items.size(); ++i) {
        const auto& f = items[i];
        const auto& vi = f.video_info();

        jstring j_path   = env->NewStringUTF(f.path.string().c_str());
        jstring j_fname  = env->NewStringUTF(f.filename.c_str());
        jstring j_fmt    = env->NewStringUTF(vi.container.c_str());
        jstring j_codec  = env->NewStringUTF(vi.suggested_codec.c_str());
        jstring j_vcodec = env->NewStringUTF(vi.video_codec.c_str());
        jstring j_acodec = env->NewStringUTF(vi.audio_codec.c_str());
        jstring j_q      = env->NewStringUTF(batchpress::quality_label(vi.quality));

        jobject item = env->NewObject(fi_cls, fi_ctor,
            JNI_TRUE, j_path, j_fname,
            static_cast<jlong>(0), static_cast<jlong>(0), static_cast<jlong>(0),
            static_cast<jint>(f.width), static_cast<jint>(f.height),
            static_cast<jlong>(f.file_size), static_cast<jlong>(f.projected_size),
            static_cast<jdouble>(f.savings_pct),
            j_fmt, j_codec,
            static_cast<jdouble>(vi.duration_sec),
            j_vcodec, j_acodec, j_path,
            j_q, static_cast<jint>(batchpress::quality_stars(vi.quality)),
            static_cast<jint>(vi.projected_width), static_cast<jint>(vi.projected_height));

        env->SetObjectArrayElement(arr, static_cast<jsize>(i), item);
        env->DeleteLocalRef(j_path);
        env->DeleteLocalRef(j_fname);
        env->DeleteLocalRef(j_fmt);
        env->DeleteLocalRef(j_codec);
        env->DeleteLocalRef(j_vcodec);
        env->DeleteLocalRef(j_acodec);
        env->DeleteLocalRef(j_q);
        env->DeleteLocalRef(item);
    }

    double elapsed = 0;  // could measure if needed
    double total_s = 0, total_proj = 0;
    for (auto& f : items) { total_s += f.file_size; total_proj += f.projected_size; }
    double savings = (total_s > 0) ? 100.0 * (1.0 - total_proj / total_s) : 0.0;

    jclass report_cls = env->FindClass("com/batchpress/FileScanReport");
    jmethodID r_ctor = env->GetMethodID(report_cls, "<init>", "([Ljava/lang/Object;DDDII)V");
    return env->NewObject(report_cls, r_ctor, arr,
        static_cast<jdouble>(elapsed), static_cast<jdouble>(savings),
        static_cast<jdouble>(total_s), static_cast<jint>(0), static_cast<jint>(items.size()));
}
#endif // BATCHPRESS_HAS_VIDEO

// ══════════════════════════════════════════════════════════════════════════════
//  UTILITY — diskFreeBytes()
// ══════════════════════════════════════════════════════════════════════════════

JNIEXPORT jlong JNICALL
Java_com_batchpress_BatchPress_diskFreeBytes(
    JNIEnv* env, jclass, jstring j_path)
{
    const char* c = env->GetStringUTFChars(j_path, nullptr);
    uint64_t free = batchpress::disk_free_bytes(batchpress::fs::path(c));
    env->ReleaseStringUTFChars(j_path, c);
    return static_cast<jlong>(free);
}

} // extern "C"
