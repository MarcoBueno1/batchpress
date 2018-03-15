# batchpress

> **Parallel media compressor written in C++17.**  
> Batch compress images and videos in-place or to a separate directory,  
> with adaptive codec selection, dry-run projection and per-directory scan reports.

![CI](https://github.com/YOUR_USERNAME/batchpress/actions/workflows/ci.yml/badge.svg)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20Android-lightgrey)

---

## What it does

`batchpress` recursively scans a directory, compresses every image and video it finds, and writes the results either in-place or to a separate output directory.

```bash
# Scan first — see projected savings before touching anything
batchpress --input /sdcard/DCIM --scan-all

# Apply in-place (default)
batchpress --input /sdcard/DCIM

# Copy to output directory
batchpress --input ./raw/ --output ./compressed/
```

---

## Quick demo

```
$ batchpress --input ./media/ --scan-all

  Codecs: H.265 ✓  H.264 ✓  VP9 ✓  AAC ✓  Opus ✓

── Images ──────────────────────────────────────────────
┌─ /media/photos/  (2847 images  │  1.8 GB)
│  Best: WebP q85 fit:1920x1080
│  ████████████████████  1.8 GB → 213 MB  save 88%  ★★★
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
║                                                      ║
║  To apply:                                           ║
║  batchpress --input ./media/                        ║
╚══════════════════════════════════════════════════════╝
```

---

## Features

| | |
|---|---|
| **Images** | Resize, convert and compress JPG/PNG/BMP/WebP in parallel |
| **Videos** | Transcode with H.265 › H.264 › VP9 — auto-selects best available codec |
| **Audio** | Classifies speech/music/silent → picks optimal bitrate automatically |
| **Scan mode** | Analyses directories and projects savings before writing anything |
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
│   │   ├── types.hpp            ← Config, TaskResult, BatchReport
│   │   ├── processor.hpp        ← Image pipeline API
│   │   ├── scanner.hpp          ← Image scan API
│   │   ├── video_processor.hpp  ← Video pipeline + scan API
│   │   ├── thread_pool.hpp      ← Header-only thread pool
│   │   └── export.hpp           ← Symbol visibility macros
│   └── src/
│       ├── types.cpp
│       ├── processor.cpp        ← stb_image + adaptive write
│       ├── scanner.cpp          ← Per-dir sampling + candidate ranking
│       └── video_processor.cpp  ← libav encode/decode + audio classification
│
└── ui/
    ├── cli/                     ← batchpress executable (Linux/Windows)
    ├── android/                 ← libbatchpress_jni.so + BatchPress.java
    └── qt/                      ← Qt6 desktop GUI (placeholder)
```

The core library has **zero UI code**. All progress is delivered via a callback:

```cpp
batchpress::Config cfg;
cfg.input_dir = "/path/to/media";

cfg.on_progress = [](const batchpress::TaskResult& res,
                     uint32_t done, uint32_t total) {
    // CLI → print to terminal
    // Qt  → emit signal to QProgressBar
    // Android → call Java via JNI
};

batchpress::BatchReport report = batchpress::run_batch(cfg);
```

---

## Build

### Linux / macOS

```bash
# Install dependencies
sudo apt install libavcodec-dev libavformat-dev libavutil-dev \
                 libswscale-dev libswresample-dev libgtest-dev cmake ninja-build

# Download stb headers
curl -sL https://raw.githubusercontent.com/nothings/stb/master/stb_image.h \
     -o core/third_party/stb_image.h
curl -sL https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h \
     -o core/third_party/stb_image_write.h
curl -sL https://raw.githubusercontent.com/nothings/stb/master/stb_image_resize2.h \
     -o core/third_party/stb_image_resize2.h

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

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
vcpkg install ffmpeg
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
  --samples <n>       Scan: images sampled per directory (default: 5)
  --verbose           Detailed output
  --help              Show help
```

---

## Android Java API

```kotlin
import com.batchpress.BatchPress

// In a coroutine or background thread:
val result = BatchPress.runBatch(
    inputDir  = "/sdcard/DCIM",
    outputDir = "",              // empty = in-place
    resize    = "fit:1920x1080",
    format    = "webp",
    quality   = 85,
    threads   = 0,               // 0 = all cores
    dryRun    = false,
    listener  = { filename, success, skipped, done, total, inBytes, outBytes, dry ->
        runOnUiThread {
            progressBar.progress = ((done * 100) / total).toInt()
        }
    }
)
```

---

## License

MIT © [Marco Antônio Bueno da Silva](mailto:bueno.mario@gmail.com)

---

## Portfolio

| Project | Skills demonstrated |
|---|---|
| **batchpress** ← you are here | Concurrency · libav · shared library · Android NDK · CLI design |
| [order-book](../order-book) | Lock-free structures · trading systems |
| [http-server](../http-server) | TCP sockets · event loop · HTTP/1.1 |
| [json-parser](../json-parser) | Recursive descent · templates · zero-alloc |

➡️ **[Full portfolio →](https://YOUR_USERNAME.github.io/cpp-portfolio)**
