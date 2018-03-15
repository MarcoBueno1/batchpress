// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.mario@gmail.com>
 *
 * This file is part of batchpress — Android JNI bridge.
 *
 * Connects Java/Kotlin code to libbatchpress_core.so via the JNI interface.
 * The core library is completely unmodified — this file is the only
 * Android-specific code in the entire project.
 *
 * Java side: com.batchpress.BatchPress
 * Usage:
 *   System.loadLibrary("batchpress_jni");
 *   BatchPress.runBatch(inputDir, outputDir, resize, format, quality, threads,
 *                     dryRun, callback);
 */

#include <jni.h>
#include <android/log.h>
#include <batchpress/processor.hpp>
#include <string>

#define LOG_TAG  "batchpress"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── JNI name mangling helper ──────────────────────────────────────────────────
// Matches package com.batchpress → Java_com_batchpress_BatchPress_*

extern "C" {

// ── BatchPress.runBatch() ───────────────────────────────────────────────────────
/**
 * Signature (Java):
 *
 * public static native BatchResult runBatch(
 *     String inputDir,
 *     String outputDir,   // null or "" = in-place
 *     String resize,      // "1920x1080" | "50%" | "fit:800x600" | ""
 *     String format,      // "jpg" | "png" | "webp" | "same"
 *     int    quality,     // 1-100
 *     int    threads,     // 0 = hardware_concurrency
 *     boolean dryRun,
 *     ProgressListener listener
 * );
 */
JNIEXPORT jobject JNICALL
Java_com_batchpress_BatchPress_runBatch(
    JNIEnv*  env,
    jclass   /*clazz*/,
    jstring  j_input_dir,
    jstring  j_output_dir,
    jstring  j_resize,
    jstring  j_format,
    jint     j_quality,
    jint     j_threads,
    jboolean j_dry_run,
    jobject  j_listener)   // com.batchpress.ProgressListener
{
    // ── Convert Java strings to std::string ──────────────────────────────
    auto jstr = [&](jstring js) -> std::string {
        if (!js) return "";
        const char* c = env->GetStringUTFChars(js, nullptr);
        std::string s(c);
        env->ReleaseStringUTFChars(js, c);
        return s;
    };

    std::string input_dir  = jstr(j_input_dir);
    std::string output_dir = jstr(j_output_dir);
    std::string resize_str = jstr(j_resize);
    std::string format_str = jstr(j_format);

    LOGI("runBatch: input=%s output=%s resize=%s format=%s quality=%d threads=%d dry=%d",
         input_dir.c_str(), output_dir.c_str(), resize_str.c_str(),
         format_str.c_str(), (int)j_quality, (int)j_threads, (int)j_dry_run);

    // ── Build Config ──────────────────────────────────────────────────────
    batchpress::Config cfg;
    cfg.input_dir   = input_dir;
    cfg.output_dir  = output_dir; // empty = in-place
    cfg.quality     = static_cast<int>(j_quality);
    cfg.num_threads = static_cast<size_t>(j_threads);
    cfg.dry_run     = static_cast<bool>(j_dry_run);

    try {
        cfg.resize = batchpress::parse_resize(resize_str);
        cfg.format = batchpress::parse_format(format_str);
    } catch (const std::exception& ex) {
        env->ThrowNew(
            env->FindClass("java/lang/IllegalArgumentException"), ex.what());
        return nullptr;
    }

    // ── Progress callback → Java ProgressListener.onProgress() ───────────
    // We capture the JavaVM so the callback can attach the worker thread.
    JavaVM* jvm = nullptr;
    env->GetJavaVM(&jvm);

    // Global ref so the listener survives across threads
    jobject g_listener = env->NewGlobalRef(j_listener);

    // Cache method ID for onProgress
    jclass   listener_cls = env->GetObjectClass(j_listener);
    jmethodID on_progress_id = env->GetMethodID(
        listener_cls,
        "onProgress",
        "(Ljava/lang/String;ZIIJJZ)V"
        // (inputPath, success, skipped, done, total, inputBytes, outputBytes, dryRun)
    );

    cfg.on_progress = [jvm, g_listener, on_progress_id]
        (const batchpress::TaskResult& res, uint32_t done, uint32_t total)
    {
        JNIEnv* cb_env = nullptr;
        bool attached  = false;

        // Worker threads must attach to the JVM before calling Java
        jint status = jvm->GetEnv(reinterpret_cast<void**>(&cb_env), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            jvm->AttachCurrentThread(&cb_env, nullptr);
            attached = true;
        }

        if (cb_env && on_progress_id) {
            jstring j_path = cb_env->NewStringUTF(
                res.input_path.filename().string().c_str());

            cb_env->CallVoidMethod(
                g_listener, on_progress_id,
                j_path,
                static_cast<jboolean>(res.success),
                static_cast<jboolean>(res.skipped),
                static_cast<jlong>(done),
                static_cast<jlong>(total),
                static_cast<jlong>(res.input_bytes),
                static_cast<jlong>(res.output_bytes),
                static_cast<jboolean>(res.dry_run)
            );

            cb_env->DeleteLocalRef(j_path);
        }

        if (attached) jvm->DetachCurrentThread();
    };

    // ── Run core ──────────────────────────────────────────────────────────
    batchpress::BatchReport report;
    try {
        report = batchpress::run_batch(cfg);
    } catch (const std::exception& ex) {
        LOGE("run_batch exception: %s", ex.what());
        env->DeleteGlobalRef(g_listener);
        env->ThrowNew(
            env->FindClass("java/lang/RuntimeException"), ex.what());
        return nullptr;
    }

    env->DeleteGlobalRef(g_listener);

    // ── Build Java BatchResult object ─────────────────────────────────────
    jclass result_cls = env->FindClass("com/batchpress/BatchResult");
    jmethodID ctor = env->GetMethodID(result_cls, "<init>", "(IIIIIIJJDZ)V");

    return env->NewObject(
        result_cls, ctor,
        static_cast<jint>(report.total),
        static_cast<jint>(report.succeeded),
        static_cast<jint>(report.skipped),
        static_cast<jint>(report.failed),
        static_cast<jint>(report.written_safe),
        static_cast<jint>(report.written_direct),
        static_cast<jlong>(report.input_bytes_total),
        static_cast<jlong>(report.output_bytes_total),
        static_cast<jdouble>(report.elapsed_sec),
        static_cast<jboolean>(report.dry_run)
    );
}

// ── BatchPress.diskFreeBytes() ──────────────────────────────────────────────────

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
