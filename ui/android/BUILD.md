# batchpress — Android Build

## How to build libbatchpress_core.so + libbatchpress_jni.so for Android

### Prerequisites

- Android NDK r25+
- CMake 3.16+
- ABI targets: `arm64-v8a` (modern devices) and/or `x86_64` (emulator)

### Build command

```bash
cmake -B build-android \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build-android --parallel
```

Output:
```
build-android/
├── libbatchpress_core.so   ← core library
└── libbatchpress_jni.so    ← JNI bridge (loaded by Java)
```

### Android Studio integration

Copy both `.so` files into your Android project:

```
app/src/main/jniLibs/
└── arm64-v8a/
    ├── libbatchpress_core.so
    └── libbatchpress_jni.so
```

Copy the Java file:
```
app/src/main/java/com/batchpress/BatchPress.java
```

Then call from Kotlin:
```kotlin
import com.batchpress.BatchPress

// In a coroutine or background thread:
val result = BatchPress.runBatch(
    inputDir  = "/sdcard/DCIM",
    outputDir = "",              // in-place
    resize    = "fit:1920x1080",
    format    = "webp",
    quality   = 85,
    threads   = 0,
    dryRun    = false,
    listener  = { filename, success, skipped, done, total, inBytes, outBytes, dry ->
        runOnUiThread { progressBar.progress = ((done * 100) / total).toInt() }
    }
)
```

### Permissions required (AndroidManifest.xml)

```xml
<uses-permission android:name="android.permission.READ_EXTERNAL_STORAGE" />
<uses-permission android:name="android.permission.WRITE_EXTERNAL_STORAGE" />
<!-- Android 13+ -->
<uses-permission android:name="android.permission.READ_MEDIA_IMAGES" />
```
