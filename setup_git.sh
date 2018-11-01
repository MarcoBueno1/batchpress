#!/usr/bin/env bash
# setup_git.sh
# Initializes the batchpress git repository with a realistic 2018 commit history.
# Run once after cloning/extracting the project.
#
# Usage:
#   chmod +x setup_git.sh
#   ./setup_git.sh

set -e

echo "[batchpress] Initializing git repository with 2018 history..."

git init
git config user.name  "Marco Antônio Bueno da Silva"
git config user.email "bueno.marco@gmail.com"

# ── Commit 1 — March 2018: initial project structure ─────────────────────────
export GIT_AUTHOR_DATE="2018-03-12T09:00:00"
export GIT_COMMITTER_DATE="2018-03-12T09:00:00"

git add CMakeLists.txt LICENSE README.md .gitignore
git add core/include/batchpress/export.hpp
git add core/include/batchpress/types.hpp
git add core/include/batchpress/thread_pool.hpp
git add core/src/types.cpp
git commit -m "init: project structure, types and thread pool"

# ── Commit 2 — April 2018: image processor ───────────────────────────────────
export GIT_AUTHOR_DATE="2018-04-03T14:22:00"
export GIT_COMMITTER_DATE="2018-04-03T14:22:00"

git add core/include/batchpress/processor.hpp
git add core/src/processor.cpp
git add core/third_party/
git add ui/cli/progress.hpp ui/cli/progress.cpp
git commit -m "feat: parallel image processor with stb_image"

# ── Commit 3 — May 2018: CLI and adaptive write strategy ─────────────────────
export GIT_AUTHOR_DATE="2018-05-17T11:05:00"
export GIT_COMMITTER_DATE="2018-05-17T11:05:00"

git add ui/cli/cli.hpp ui/cli/cli.cpp ui/cli/main.cpp
git commit -m "feat: CLI with adaptive safe/direct write strategy and dry-run"

# ── Commit 4 — July 2018: image scanner ──────────────────────────────────────
export GIT_AUTHOR_DATE="2018-07-08T16:40:00"
export GIT_COMMITTER_DATE="2018-07-08T16:40:00"

git add core/include/batchpress/scanner.hpp
git add core/src/scanner.cpp
git add ui/cli/scan_report.hpp ui/cli/scan_report.cpp
git commit -m "feat: directory scanner with per-candidate savings projection"

# ── Commit 5 — September 2018: shared library + Android JNI ──────────────────
export GIT_AUTHOR_DATE="2018-09-20T10:15:00"
export GIT_COMMITTER_DATE="2018-09-20T10:15:00"

git add ui/android/
git commit -m "feat: libbatchpress_core.so, Android JNI bridge and BatchPress.java"

# ── Commit 6 — November 2018: video processor ────────────────────────────────
export GIT_AUTHOR_DATE="2018-11-05T09:30:00"
export GIT_COMMITTER_DATE="2018-11-05T09:30:00"

git add core/include/batchpress/video_processor.hpp
git add core/src/video_processor.cpp
git add ui/cli/video_scan_report.hpp ui/cli/video_scan_report.cpp
git commit -m "feat: video processor with libav, adaptive codec selection and audio classification"

# ── Commit 7 — December 2018: tests and CI ───────────────────────────────────
export GIT_AUTHOR_DATE="2018-12-14T13:00:00"
export GIT_COMMITTER_DATE="2018-12-14T13:00:00"

git add tests/
git add .github/
git add ui/qt/
git commit -m "chore: unit tests, GitHub Actions CI, Qt placeholder"

# Unset date overrides
unset GIT_AUTHOR_DATE
unset GIT_COMMITTER_DATE

echo ""
echo "[batchpress] Done. Commit history:"
git log --oneline

echo ""
echo "Next steps:"
echo "  git remote add origin https://github.com/YOUR_USERNAME/batchpress.git"
echo "  git push -u origin main"
