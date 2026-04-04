# batchpress

> **Parallel media compressor written in C++17.**
> Batch compress images and videos in-place or to a separate directory,
> with adaptive codec selection, dry-run projection and per-directory scan reports.

![CI](https://github.com/MarcoBueno1/batchpress/actions/workflows/ci.yml/badge.svg)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20Android-lightgrey)

---

## What it does

`batchpress` recursively scans a directory, compresses every image and video it finds, and writes the results either in-place or to a separate output directory.

```bash
# Scan first — see projected savings before touching anything
batchpress --input /sdcard/DCIM --scan-all

# Interactive select — pick files, then process
batchpress --input /sdcard/DCIM --select

# Apply in-place (default)
batchpress --input /sdcard/DCIM

# Copy to output directory
batchpress --input ./raw/ --output ./compressed/
```

---

## Quick demo

### Scan mode

```
$ batchpress --input ./media/ --scan-all

  Codecs: H.265 ✓  H.264 ✓  VP9 ✓  AAC ✓  Opus ✓

── Images ──────────────────────────────────────────────
┌─ /media/photos/  (2847 images  │  1.8 GB)
│  Best: WebP q60 50%
│  ████████████████████  1.8 GB → 44 KB  save 98%  ★★★
└

── Videos ──────────────────────────────────────────────
┌─ /media/videos/  (124 videos  │  48.3 GB  │  6h 12m)
│  Codec: H.265  CRF 28
│  48.3 GB → 19.3 GB  save 60%  ★★
└

╔══════════════════════════════════════════════════════╗
║  COMBINED TOTAL                                      ║
║  Current total         50.1 GB                      ║
║  Total freed       31.3 GB  (62%)                   ║
╚══════════════════════════════════════════════════════╝
```

### Interactive select mode

```
$ batchpress --input ./media/ --select

  batchpress — Select Files to Process

  Filter: (type to filter)
  15/15 selected  |  2.9 MB → 393 KB

  [✓] photo_4k_gradient.webp       841 KB  96%  IMG
  [✓] photo_fullhd_circles.webp    283 KB  95%  IMG
  [✓] video_1080p_h264_speech.mp4   12 MB  60%  VID
  >[✓] photo_hd_stripes.webp        52 KB  94%  IMG

  ↑↓/kj:navigate  Space:toggle  a:all  i:invert  Enter:process  q:quit
```

---

## Features

| | |
|---|---|
| **Images** | Resize, convert and compress JPG/PNG/BMP/WebP in parallel |
| **Videos** | Transcode with H.265 › H.264 › VP9 — auto-selects best available codec |
| **Audio** | Classifies speech/music/silent → picks optimal bitrate automatically |
| **Scan mode** | Analyses directories and projects savings before writing anything |
| **Select mode** | Interactive TUI — pick individual files to process with Space/Enter |
| **Dry-run** | Full encode in RAM — accurate projection, nothing written to disk |
| **In-place** | Adaptive write strategy: Safe (atomic rename) or Direct (zero extra space) |
| **Parallel** | Custom C++17 thread pool — saturates all CPU cores |
| **Portable** | Compiles on Linux, Windows, macOS and Android NDK (via ffmpeg-kit) |
| **No UI in core** | `libbatchpress_core.so` has zero stdout/stderr — pure callback API |

---

## Architecture

```
batchpress/
├── core/                        ← libbatchpress_core.so
│   ├── include/batchpress/
│   │   ├── types.hpp            ← Config, FileItem, TaskResult, BatchReport
│   │   ├── processor.hpp        ← Image pipeline API
│   │   ├── scanner.hpp          ← Image scan + scan_files() API
│   │   ├── video_processor.hpp  ← Video pipeline + scan API
│   │   ├── thread_pool.hpp      ← Header-only thread pool
│   │   └── export.hpp           ← Symbol visibility macros
│   └── src/
│       ├── types.cpp
│       ├── sha256.hpp           ← Streaming SHA-256 (shared)
│       ├── processor.cpp        ← stb_image + adaptive write + WebP
│       ├── scanner.cpp          ← Per-file scan + candidate ranking
│       └── video_processor.cpp  ← libav encode/decode + audio classification
│
└── ui/
    ├── cli/                     ← batchpress executable (Linux/Windows)
    │   ├── main.cpp             ← Mode dispatcher
    │   ├── cli.cpp              ← Argument parser + help
    │   ├── select.cpp           ← Interactive TUI (terminal UI)
    │   ├── progress.cpp         ← Progress bar
    │   └── scan_report.cpp      ← Scan report formatting
    ├── android/                 ← libbatchpress_jni.so + BatchPress.java
    └── qt/                      ← Qt6 desktop GUI (placeholder)
```

---

## Library API

The core library has **zero UI code**. All progress is delivered via a callback:

### Traditional batch (auto-process everything)

```cpp
batchpress::Config cfg;
cfg.input_dir = "/path/to/media";
cfg.resize    = batchpress::parse_resize("fit:1920x1080");
cfg.format    = batchpress::ImageFormat::WebP;
cfg.quality   = 85;

cfg.on_progress = [](const batchpress::TaskResult& res,
                     uint64_t done, uint64_t total) {
    // CLI → print to terminal
    // Qt  → emit signal to QProgressBar
    // Android → call Java via JNI
};

batchpress::BatchReport report = batchpress::run_batch(cfg);
```

### Selective processing (scan → user picks → process)

```cpp
// 1. Scan all files with projected savings
batchpress::ScanConfig scan_cfg;
scan_cfg.root_dir = "/path/to/media";
scan_cfg.recursive = true;
auto report = batchpress::scan_files(scan_cfg);

// report.files now contains FileItem with:
//   filename, path, timestamps, type, dimensions, file_size
//   projected_size, savings_pct, suggested_codec

// 2. Let the UI filter / let user pick which files to process
std::vector<batchpress::FileItem> selected;
for (const auto& f : report.files) {
    if (f.savings_pct > 50.0)  // example filter
        selected.push_back(f);
}

// 3. Process only the selected files
batchpress::Config cfg;
cfg.format  = batchpress::ImageFormat::WebP;
cfg.quality = 85;
auto result = batchpress::process_files(selected, cfg);

// Same for videos:
auto vid_result = batchpress::process_video_files(selected, vid_cfg);
```

---

## Build

### Linux / macOS

```bash
# Install dependencies
sudo apt install libavcodec-dev libavformat-dev libavutil-dev \
                 libswscale-dev libswresample-dev libwebp-dev \
                 libgtest-dev cmake ninja-build

# Download stb headers (auto-downloaded by CMake if missing)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

> **Note:** WebP support requires `libwebp-dev`. Without it, WebP encoding falls back to PNG.

### Android NDK (via ffmpeg-kit)

```bash
# Download ffmpeg-kit full package from:
# https://github.com/arthenica/ffmpeg-kit/releases

cmake -B build-android \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DFFMPEG_KIT_DIR=/path/to/ffmpeg-kit-full \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build-android --parallel
# Output: libbatchpress_core.so + libbatchpress_jni.so
```

### Windows (vcpkg)

```powershell
vcpkg install ffmpeg libwebp
cmake -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

---

## Usage

```
batchpress --input <dir> [options]

MODES:
  (default)           Process images + videos in-place
  --dry-run           Encode in RAM. Print projected savings. Nothing written.
  --scan              Analyse images → suggest best format/resize
  --scan-video        Analyse videos → detect best codec/CRF for this system
  --scan-all          Analyse both images and videos
  --select            Interactive file selection — pick files, then process

SELECT OPTIONS:
  --filter <type>     image | video | all  (only show matching types)
  --min-savings <pct> Only show files with >= this savings %

OUTPUT:
  --output <dir>      Write to separate directory (disables in-place)

VIDEO OPTIONS:
  --vcodec <codec>    h265 | h264 | vp9 | auto  (default: auto)
  --crf <n>           Quality: lower = better. Default: h265=28 h264=26 vp9=33
  --max-res <res>     1080p | 4k | original      (default: 1080p)

IMAGE OPTIONS:
  --resize  <spec>    1920x1080 | 50% | fit:1280x720
  --format  <fmt>     jpg | png | bmp | webp | same
  --quality <1-100>   JPEG/WebP quality (default: 90)

COMMON:
  --threads <n>       Worker threads (default: CPU cores)
  --no-recursive      Do not traverse subdirectories
  --overwrite         Overwrite existing output files
  --no-dedup          Disable duplicate detection (default: enabled)
  --samples <n>       Scan: images sampled per directory (default: 5)
  --verbose           Detailed output
  --help              Show help
```

### Interactive select controls

| Key | Action |
|-----|--------|
| `↑` `↓` or `k` `j` | Navigate file list |
| `Space` | Toggle selection of current file |
| `a` | Select / Deselect all |
| `i` | Invert selection |
| `Enter` | Process selected files |
| `q` / `Ctrl+C` | Quit without processing |
| *type any text* | Filter by filename substring |

---

## Android Java API

The complete core library is exposed via JNI. All methods block — call from a
background thread (Kotlin coroutine, ExecutorService, RxJava).

### Scan files → user picks → process

```kotlin
import com.batchpress.*

viewModelScope.launch(Dispatchers.IO) {
    // 1. Scan images with per-file projections
    val imgScan = BatchPress.scanFiles(
        rootDir   = "/sdcard/DCIM/Camera",
        recursive = true,
        samples   = 5,
        threads   = 0,
        listener  = object : BatchPress.ScanProgressListener {
            override fun onProgress(name: String, done: Int, total: Int) {
                progressBar.progress = ((done * 100) / total)
            }
        }
    )

    // 2. Scan videos with per-file projections
    val vidScan = BatchPress.scanVideoFiles(
        rootDir   = "/sdcard/DCIM/Camera",
        recursive = true,
        vcodec    = "h265",
        crf       = 28,           // -1 = auto
        maxRes    = "1080p",
        audioBps  = -1,           // auto
        threads   = 0,
        listener  = object : BatchPress.ScanProgressListener {
            override fun onProgress(name: String, done: Int, total: Int) {
                // update progress
            }
        }
    )

    // 3. Filter — only files with good quality and savings
    val imgSelected = imgScan.files.filter {
        it.savingsPct > 40.0 && it.qualityStars >= 3
    }
    val vidSelected = vidScan.files.filter {
        it.savingsPct > 50.0 && it.qualityStars >= 3
    }

    // 4. Process selected images
    val imgResult = BatchPress.processFiles(
        files     = imgSelected.toTypedArray(),
        inputDir  = "/sdcard/DCIM/Camera",
        outputDir = "",              // empty = in-place
        resize    = "fit:1920x1080",
        format    = "webp",
        quality   = 85,
        threads   = 0,
        dryRun    = false,
        dedup     = true,
        listener  = object : BatchPress.ProgressListener {
            override fun onProgress(filename: String, success: Boolean,
                skipped: Boolean, done: Int, total: Int,
                inputBytes: Long, outputBytes: Long, dryRun: Boolean) {
                // update progress
            }
        }
    )

    // 5. Process selected videos
    val vidResult = BatchPress.processVideoFiles(
        files       = vidSelected.toTypedArray(),
        inputDir    = "/sdcard/DCIM/Camera",
        outputDir   = "",
        vcodec      = "h265",
        crf         = 28,
        maxRes      = "1080p",
        audioBps    = -1,
        threads     = 0,
        dryRun      = false,
        dedup       = true,
        listener    = object : BatchPress.VideoProgressListener {
            override fun onProgress(path: String, frameDone: Long,
                frameTotal: Long, filesDone: Int, filesTotal: Int) {
                // per-frame progress
            }
        }
    )
}
```

### Traditional batch (no selection)

```kotlin
// Images
val imgResult = BatchPress.runBatch(
    inputDir  = "/sdcard/DCIM",
    outputDir = "",
    resize    = "fit:1920x1080",
    format    = "webp",
    quality   = 85,
    threads   = 0,
    dryRun    = false,
    dedup     = true,
    listener  = { filename, success, skipped, done, total, inBytes, outBytes, dry ->
        progressBar.progress = ((done * 100) / total)
    }
)

// Videos
val vidResult = BatchPress.runVideoBatch(
    inputDir  = "/sdcard/DCIM",
    outputDir = "",
    vcodec    = "h265",
    crf       = 28,
    maxRes    = "1080p",
    audioBps  = -1,
    threads   = 0,
    dryRun    = false,
    dedup     = true,
    listener  = { path, frameDone, frameTotal, filesDone, filesTotal ->
        // per-frame progress
    }
)
```

### Legacy per-directory scan

```kotlin
val imgSummary = BatchPress.runScan("/sdcard/DCIM", true, 5, 0, null)
println("Best config: ${imgSummary.bestConfig}")
println("Savings: ${imgSummary.savingsPct}%")

val vidSummary = BatchPress.runVideoScan("/sdcard/DCIM", true, 0, null)
println("Suggested codec: ${vidSummary.suggestedCodec} CRF${vidSummary.suggestedCrf}")
```

### Available Java classes

| Class | Purpose |
|-------|---------|
| `FileItem` | Per-file metadata: path, type, dimensions, size, projected savings, **quality**, **projected resolution** |
| `FileScanReport` | scanFiles() / scanVideoFiles() result: FileItem[], counts, totals |
| `BatchResult` | Image batch result: total, succeeded, failed, bytes saved |
| `VideoBatchResult` | Video batch result: codec usage breakdown, bytes saved |
| `ScanSummary` | Legacy per-directory image scan summary |
| `VideoScanSummary` | Legacy per-directory video scan summary |

### FileItem fields

| Field | Type | Description |
|-------|------|-------------|
| `path` | String | Absolute file path |
| `filename` | String | File name only |
| `width`, `height` | int | Original resolution |
| `fileSize` | long | Original file size in bytes |
| `projectedSize` | long | Estimated size after compression |
| `savingsPct` | double | Estimated savings percentage (0-100) |
| `qualityLabel` | String | "Lossless", "High", "Medium", "Low" |
| `qualityStars` | int | 1-5 star rating |
| `projectedWidth`, `projectedHeight` | int | Output resolution (0 = same as original) |
| `durationSec` | double | Video duration (0 for images) |
| `videoCodec`, `audioCodec` | String | Current codec names (videos only) |
| `suggestedCodec` | String | e.g. "WebP q85 fit:1920x1080" or "H.265 CRF28" |

---

## License

MIT © [Marco Antônio Bueno da Silva](mailto:bueno.marco@gmail.com)
