// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.marco@gmail.com>
 *
 * This file is part of batchpress — Android Java API.
 *
 * Thin Java wrapper around the JNI bridge to libbatchpress_core.so.
 * Call runBatch() from a background thread (e.g. ViewModel + coroutine,
 * or ExecutorService) — it blocks until all images are processed.
 */

package com.batchpress;

/**
 * Entry point for the batchpress core library on Android.
 *
 * <pre>
 * // Usage example (Kotlin coroutine):
 * viewModelScope.launch(Dispatchers.IO) {
 *     val result = BatchPress.runBatch(
 *         inputDir  = "/sdcard/DCIM/Camera",
 *         outputDir = "",            // empty = in-place
 *         resize    = "fit:1920x1080",
 *         format    = "webp",
 *         quality   = 85,
 *         threads   = 0,             // 0 = hardware_concurrency
 *         dryRun    = false,
 *         listener  = object : BatchPress.ProgressListener {
 *             override fun onProgress(
 *                 filename: String, success: Boolean, skipped: Boolean,
 *                 done: Long, total: Long,
 *                 inputBytes: Long, outputBytes: Long, dryRun: Boolean
 *             ) {
 *                 withContext(Dispatchers.Main) {
 *                     progressBar.progress = ((done * 100) / total).toInt()
 *                 }
 *             }
 *         }
 *     )
 *     withContext(Dispatchers.Main) { showReport(result) }
 * }
 * </pre>
 */
public class BatchPress {

    static {
        System.loadLibrary("batchpress_jni");
    }

    // ── Progress callback interface ───────────────────────────────────────────

    /**
     * Called from a native worker thread after each image is processed.
     * Dispatch to the UI thread manually if you need to update Views.
     */
    public interface ProgressListener {
        /**
         * @param filename    Filename of the processed image
         * @param success     True if processing succeeded
         * @param skipped     True if file was skipped (already exists)
         * @param done        Number of images finished so far
         * @param total       Total number of images in the batch
         * @param inputBytes  Original file size in bytes
         * @param outputBytes Resulting file size in bytes (estimated in dry-run)
         * @param dryRun      True if this was a dry-run (nothing written)
         */
        void onProgress(
            String  filename,
            boolean success,
            boolean skipped,
            long    done,
            long    total,
            long    inputBytes,
            long    outputBytes,
            boolean dryRun
        );
    }

    // ── BatchResult ───────────────────────────────────────────────────────────

    public static class BatchResult {
        public final int     total;
        public final int     succeeded;
        public final int     skipped;
        public final int     failed;
        public final int     writtenSafe;
        public final int     writtenDirect;
        public final long    inputBytesTotal;
        public final long    outputBytesTotal;
        public final double  elapsedSec;
        public final boolean dryRun;

        /** Called from JNI — do not rename parameters. */
        public BatchResult(
            int total, int succeeded, int skipped, int failed,
            int writtenSafe, int writtenDirect,
            long inputBytesTotal, long outputBytesTotal,
            double elapsedSec, boolean dryRun)
        {
            this.total            = total;
            this.succeeded        = succeeded;
            this.skipped          = skipped;
            this.failed           = failed;
            this.writtenSafe      = writtenSafe;
            this.writtenDirect    = writtenDirect;
            this.inputBytesTotal  = inputBytesTotal;
            this.outputBytesTotal = outputBytesTotal;
            this.elapsedSec       = elapsedSec;
            this.dryRun           = dryRun;
        }

        public long bytesSaved() {
            return inputBytesTotal - outputBytesTotal;
        }

        public double savingsPct() {
            if (inputBytesTotal == 0) return 0.0;
            return 100.0 * (1.0 - (double) outputBytesTotal / inputBytesTotal);
        }

        public double throughput() {
            return elapsedSec > 0.0 ? succeeded / elapsedSec : 0.0;
        }

        @Override
        public String toString() {
            return String.format(
                "BatchResult{total=%d, succeeded=%d, skipped=%d, failed=%d, " +
                "safe=%d, direct=%d, saved=%.1f%%, elapsed=%.2fs}",
                total, succeeded, skipped, failed,
                writtenSafe, writtenDirect, savingsPct(), elapsedSec);
        }
    }

    // ── Native methods ────────────────────────────────────────────────────────

    /**
     * Runs the full batch pipeline. Blocking — call from a background thread.
     *
     * @param inputDir   Absolute path to source directory
     * @param outputDir  Absolute path to output directory, or "" for in-place
     * @param resize     Resize spec: "1920x1080" | "50%" | "fit:800x600" | ""
     * @param format     Output format: "jpg" | "png" | "bmp" | "webp" | "same"
     * @param quality    JPEG/WebP quality 1-100 (ignored for PNG/BMP)
     * @param threads    Worker thread count (0 = use all CPU cores)
     * @param dryRun     If true, process in RAM only — nothing written to disk
     * @param listener   Progress callback, called after each image
     * @return           Aggregated result report
     */
    public static native BatchResult runBatch(
        String           inputDir,
        String           outputDir,
        String           resize,
        String           format,
        int              quality,
        int              threads,
        boolean          dryRun,
        ProgressListener listener
    );

    /**
     * Returns available disk space in bytes for the filesystem at @p path.
     */
    public static native long diskFreeBytes(String path);
}
