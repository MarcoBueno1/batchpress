// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.marco@gmail.com>
 *
 * This file is part of batchpress — Android Java API.
 *
 * Thin Java wrapper around the JNI bridge to libbatchpress_core.so.
 *
 * Exposes ALL core library functionality:
 *   - runBatch()          — traditional image batch processing
 *   - runVideoBatch()     — traditional video batch processing
 *   - scanFiles()         — per-file scan with projected savings
 *   - processFiles()      — selective image processing
 *   - processVideoFiles() — selective video processing
 *   - runScan()           — legacy per-directory image scan
 *   - runVideoScan()      — legacy per-directory video scan
 *   - diskFreeBytes()     — available disk space
 *
 * Call any method from a background thread (e.g. Kotlin coroutine,
 * ExecutorService, or RxJava) — all methods block until complete.
 */

package com.batchpress;

/**
 * Entry point for the batchpress core library on Android.
 *
 * <pre>
 * // Example: scan files, let user pick, then process (Kotlin coroutine)
 * viewModelScope.launch(Dispatchers.IO) {
 *     val scan = BatchPress.scanFiles(
 *         rootDir   = "/sdcard/DCIM",
 *         recursive = true,
 *         samples   = 5,
 *         threads   = 0,
 *         listener  = object : BatchPress.ScanProgressListener {
 *             override fun onProgress(name: String, done: Int, total: Int) {
 *                 // update progress bar
 *             }
 *         }
 *     )
 *
 *     // Filter files with >50% savings
 *     val selected = scan.files.filter { it.savingsPct > 50.0 }
 *
 *     // Process selected images
 *     val imgResult = BatchPress.processFiles(
 *         files     = selected.toTypedArray(),
 *         inputDir  = "/sdcard/DCIM",
 *         outputDir = "",
 *         resize    = "fit:1920x1080",
 *         format    = "webp",
 *         quality   = 85,
 *         threads   = 0,
 *         dryRun    = false,
 *         dedup     = true,
 *         listener  = object : BatchPress.ProgressListener {
 *             override fun onProgress(filename: String, success: Boolean,
 *                 skipped: Boolean, done: Int, total: Int,
 *                 inputBytes: Long, outputBytes: Long, dryRun: Boolean) {
 *                 // update progress
 *             }
 *         }
 *     )
 *
 *     // Process selected videos
 *     val vidResult = BatchPress.processVideoFiles(
 *         files       = selected.toTypedArray(),
 *         inputDir    = "/sdcard/DCIM",
 *         outputDir   = "",
 *         vcodec      = "h265",
 *         crf         = 28,
 *         maxRes      = "1080p",
 *         audioBps    = -1,  // auto
 *         threads     = 0,
 *         dryRun      = false,
 *         dedup       = true,
 *         listener    = object : BatchPress.VideoProgressListener {
 *             override fun onProgress(path: String, frameDone: Long,
 *                 frameTotal: Long, filesDone: Int, filesTotal: Int) {
 *                 // update progress
 *             }
 *         }
 *     )
 * }
 * </pre>
 */
public class BatchPress {

    static {
        System.loadLibrary("batchpress_jni");
    }

    // ── Progress callback interfaces ───────────────────────────────────────────

    /**
     * Called from a native worker thread after each image is processed.
     */
    public interface ProgressListener {
        void onProgress(
            String  filename,
            boolean success,
            boolean skipped,
            int     done,
            int     total,
            long    inputBytes,
            long    outputBytes,
            boolean dryRun
        );
    }

    /**
     * Called from a native worker thread during video encoding.
     * Provides per-frame progress and per-file completion counts.
     */
    public interface VideoProgressListener {
        /**
         * @param path        Current video being processed
         * @param frameDone   Frames encoded so far for this file
         * @param frameTotal  Total frames in this file (0 if unknown)
         * @param filesDone   Files completed so far
         * @param filesTotal  Total files in batch
         */
        void onProgress(
            String path,
            long   frameDone,
            long   frameTotal,
            int    filesDone,
            int    filesTotal
        );
    }

    /**
     * Called from a native worker thread during file scan.
     */
    public interface ScanProgressListener {
        void onProgress(String filename, int done, int total);
    }

    // ── Result classes ────────────────────────────────────────────────────────

    /** Result of a batch image processing run. */
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
                "BatchResult{total=%d, ok=%d, skip=%d, fail=%d, saved=%.1f%%, %.2fs}",
                total, succeeded, skipped, failed, savingsPct(), elapsedSec);
        }
    }

    /** Result of a batch video processing run. */
    public static class VideoBatchResult {
        public final int     total;
        public final int     succeeded;
        public final int     skipped;
        public final int     failed;
        public final long    inputBytesTotal;
        public final long    outputBytesTotal;
        public final int     usedH265;
        public final int     usedH264;
        public final int     usedVP9;
        public final double  elapsedSec;
        public final boolean dryRun;

        public VideoBatchResult(
            int total, int succeeded, int skipped, int failed,
            long inputBytesTotal, long outputBytesTotal,
            int usedH265, int usedH264, int usedVP9,
            double elapsedSec, boolean dryRun)
        {
            this.total            = total;
            this.succeeded        = succeeded;
            this.skipped          = skipped;
            this.failed           = failed;
            this.inputBytesTotal  = inputBytesTotal;
            this.outputBytesTotal = outputBytesTotal;
            this.usedH265         = usedH265;
            this.usedH264         = usedH264;
            this.usedVP9          = usedVP9;
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

        @Override
        public String toString() {
            return String.format(
                "VideoBatchResult{total=%d, ok=%d, fail=%d, H265=%d, H264=%d, VP9=%d, saved=%.1f%%}",
                total, succeeded, failed, usedH265, usedH264, usedVP9, savingsPct());
        }
    }

    /** A single file with metadata and projected savings. */
    public static class FileItem {
        public final boolean isVideo;
        public final String  path;
        public final String  filename;
        public final long    creationTime;
        public final long    lastAccess;
        public final long    lastModified;
        public final int     width;
        public final int     height;
        public final long    fileSize;
        public final long    projectedSize;
        public final double  savingsPct;
        public final String  format;
        public final String  suggestedCodec;
        public final double  durationSec;
        public final String  videoCodec;
        public final String  audioCodec;
        public final String  displayHint;

        public FileItem(
            boolean isVideo,
            String  path,
            String  filename,
            long    creationTime,
            long    lastAccess,
            long    lastModified,
            int     width,
            int     height,
            long    fileSize,
            long    projectedSize,
            double  savingsPct,
            String  format,
            String  suggestedCodec,
            double  durationSec,
            String  videoCodec,
            String  audioCodec,
            String  displayHint)
        {
            this.isVideo       = isVideo;
            this.path          = path;
            this.filename      = filename;
            this.creationTime  = creationTime;
            this.lastAccess    = lastAccess;
            this.lastModified  = lastModified;
            this.width         = width;
            this.height        = height;
            this.fileSize      = fileSize;
            this.projectedSize = projectedSize;
            this.savingsPct    = savingsPct;
            this.format        = format;
            this.suggestedCodec = suggestedCodec;
            this.durationSec   = durationSec;
            this.videoCodec    = videoCodec;
            this.audioCodec    = audioCodec;
            this.displayHint   = displayHint;
        }

        public long bytesSaved() {
            return fileSize - projectedSize;
        }

        @Override
        public String toString() {
            return String.format("%s %s (%.0f%% saved, %s)",
                isVideo ? "VID" : "IMG", filename, savingsPct, format);
        }
    }

    /** Report returned by scanFiles(). */
    public static class FileScanReport {
        public final FileItem[] files;
        public final double     elapsedSec;
        public final double     overallSavingsPct;
        public final double     totalSize;
        public final int        imageCount;
        public final int        videoCount;

        public FileScanReport(
            FileItem[] files,
            double     elapsedSec,
            double     overallSavingsPct,
            double     totalSize,
            int        imageCount,
            int        videoCount)
        {
            this.files              = files;
            this.elapsedSec         = elapsedSec;
            this.overallSavingsPct  = overallSavingsPct;
            this.totalSize          = totalSize;
            this.imageCount         = imageCount;
            this.videoCount         = videoCount;
        }

        public long totalProjectedSize() {
            long s = 0;
            for (FileItem f : files) s += f.projectedSize;
            return s;
        }

        public long totalBytesSaved() {
            long s = 0;
            for (FileItem f : files) s += f.bytesSaved();
            return s;
        }

        public FileItem[] filterByType(boolean video) {
            java.util.ArrayList<FileItem> out = new java.util.ArrayList<>();
            for (FileItem f : files) if (f.isVideo == video) out.add(f);
            return out.toArray(new FileItem[0]);
        }

        public FileItem[] filterByMinSavings(double minPct) {
            java.util.ArrayList<FileItem> out = new java.util.ArrayList<>();
            for (FileItem f : files) if (f.savingsPct >= minPct) out.add(f);
            return out.toArray(new FileItem[0]);
        }

        @Override
        public String toString() {
            return String.format(
                "FileScanReport{files=%d, images=%d, videos=%d, saved=%.1f%%, %.2fs}",
                files.length, imageCount, videoCount, overallSavingsPct, elapsedSec);
        }
    }

    /** Simplified summary from legacy runScan(). */
    public static class ScanSummary {
        public final long   totalBytes;
        public final long   projectedBytes;
        public final long   bytesSaved;
        public final double savingsPct;
        public final String bestConfig;

        public ScanSummary(long total, long projected, long saved, double pct, String best) {
            this.totalBytes     = total;
            this.projectedBytes = projected;
            this.bytesSaved     = saved;
            this.savingsPct     = pct;
            this.bestConfig     = best;
        }
    }

    /** Simplified summary from legacy runVideoScan(). */
    public static class VideoScanSummary {
        public final long   totalBytes;
        public final long   projectedBytes;
        public final long   bytesSaved;
        public final double savingsPct;
        public final String suggestedCodec;
        public final int    suggestedCrf;

        public VideoScanSummary(long total, long projected, long saved, double pct, String codec, int crf) {
            this.totalBytes      = total;
            this.projectedBytes  = projected;
            this.bytesSaved      = saved;
            this.savingsPct      = pct;
            this.suggestedCodec  = codec;
            this.suggestedCrf    = crf;
        }
    }

    // ── Native methods ────────────────────────────────────────────────────────

    /**
     * Runs the full image batch pipeline.
     * Call from a background thread — blocks until complete.
     */
    public static native BatchResult runBatch(
        String           inputDir,
        String           outputDir,
        String           resize,
        String           format,
        int              quality,
        int              threads,
        boolean          dryRun,
        boolean          dedup,
        ProgressListener listener
    );

    /**
     * Runs the full video batch pipeline.
     * Call from a background thread — blocks until complete.
     *
     * @param vcodec    "h265" | "h264" | "vp9" | "auto"
     * @param crf       Quality: -1 = auto (H265=28, H264=26, VP9=33)
     * @param maxRes    "1080p" | "4k" | "original"
     * @param audioBps  Audio bitrate in kbps, -1 = auto
     */
    public static native VideoBatchResult runVideoBatch(
        String                inputDir,
        String                outputDir,
        String                vcodec,
        int                   crf,
        String                maxRes,
        int                   audioBps,
        int                   threads,
        boolean               dryRun,
        boolean               dedup,
        VideoProgressListener listener
    );

    /**
     * Scans all files (images + videos) and returns per-file metadata
     * with projected compression savings.
     *
     * Use this to let the user pick which files to process.
     *
     * @param samples   Samples per file for estimation (0 = full encode, 5 = good default)
     */
    public static native FileScanReport scanFiles(
        String                rootDir,
        boolean               recursive,
        int                   samples,
        int                   threads,
        ScanProgressListener  listener
    );

    /**
     * Processes a user-selected list of image files.
     *
     * The files array should come from scanFiles(). Filter/select in Java,
     * then pass the result here.
     *
     * @param files     Array of FileItem from scanFiles() — only images are processed
     * @param inputDir  Base input directory (for path resolution)
     */
    public static native BatchResult processFiles(
        FileItem[]         files,
        String             inputDir,
        String             outputDir,
        String             resize,
        String             format,
        int                quality,
        int                threads,
        boolean            dryRun,
        boolean            dedup,
        ProgressListener   listener
    );

    /**
     * Processes a user-selected list of video files.
     *
     * The files array should come from scanFiles(). Only video items are processed.
     *
     * @param files      Array of FileItem from scanFiles() — only videos are processed
     * @param vcodec     "h265" | "h264" | "vp9" | "auto"
     * @param crf        Quality: -1 = auto
     * @param maxRes     "1080p" | "4k" | "original"
     * @param audioBps   Audio bitrate in kbps, -1 = auto
     */
    public static native VideoBatchResult processVideoFiles(
        FileItem[]              files,
        String                  inputDir,
        String                  outputDir,
        String                  vcodec,
        int                     crf,
        String                  maxRes,
        int                     audioBps,
        int                     threads,
        boolean                 dryRun,
        boolean                 dedup,
        VideoProgressListener   listener
    );

    /**
     * Legacy: per-directory image scan.
     * For per-file scanning, prefer scanFiles().
     */
    public static native ScanSummary runScan(
        String               rootDir,
        boolean              recursive,
        int                  samples,
        int                  threads,
        ScanProgressListener listener
    );

    /**
     * Legacy: per-directory video scan.
     * For per-file scanning, prefer scanFiles().
     */
    public static native VideoScanSummary runVideoScan(
        String               rootDir,
        boolean              recursive,
        int                  threads,
        ScanProgressListener listener
    );

    /**
     * Returns available disk space in bytes for the filesystem at @p path.
     */
    public static native long diskFreeBytes(String path);
}
