# batchpress — Qt Frontend (placeholder)

This directory will contain the Qt6 desktop GUI frontend.

## Planned structure

```
ui/qt/
├── main_qt.cpp          ← QApplication entry point
├── MainWindow.cpp/.hpp  ← Main window with drag-drop folder selection
├── MainWindow.ui        ← Qt Designer layout
├── WorkerThread.cpp     ← Runs run_batch() in QThread
└── CMakeLists.txt       ← Qt-specific build config
```

## How it connects to the core

```cpp
// WorkerThread.cpp
#include <batchpress/processor.hpp>

batchpress::Config cfg;
cfg.input_dir   = inputPath.toStdString();
cfg.output_dir  = outputPath.toStdString();
cfg.resize      = batchpress::parse_resize(resizeStr.toStdString());
cfg.format      = batchpress::parse_format(formatStr.toStdString());
cfg.quality     = qualitySlider->value();

// Callback runs in worker thread — emit signal to update UI safely
cfg.on_progress = [this](const batchpress::TaskResult& res,
                          uint32_t done, uint32_t total) {
    emit progressUpdated(done, total,
        QString::fromStdString(res.input_path.filename().string()),
        res.input_bytes, res.output_bytes);
};

batchpress::BatchReport report = batchpress::run_batch(cfg);
emit batchFinished(report);
```

## Build

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/Qt6
cmake --build build
```
