# Phase 0 Summary — Build System Refactor

## What was done

### Task 0.1: Restructure x264 build integration
- Removed dependency on stale `include/x264/` headers (deleted directory)
- Updated Makefile include path: `-I./include` → `-Ithird_party` so `#include "x264/x264.h"` resolves to `third_party/x264/x264.h`
- Updated Makefile library path: `-L./lib` → `-L$(X264_DIR)` (pointing to `third_party/x264/`)
- Made `-ldrm` Linux-only (was causing link failure on macOS)
- Fixed C++ standard flag: `-std=c++23` → `-std=c++2b` (Apple clang 15 compatibility)
- Kept `-L./lib` conditional on VVENC=1 for vvenc/vvdec libraries

### Task 0.2: One-tap build & run scripts
- `scripts/build.sh`: Builds x264 from source (configure + make) then builds main project
- `scripts/run_local.sh`: Runs sender+receiver on localhost with no mahimahi dependency

## Verification
- `make clean && make` succeeds on macOS (ARM64, Apple clang 15)
- `scripts/build.sh` builds x264 + main project in one step
- `scripts/run_local.sh mock 30 30` runs full sender→receiver pipeline with mock codec

## Notes
- x264 was already configured+built in `third_party/x264/` (build 164)
- `include/x264/` had slightly newer headers (build 165) but the .a was build 164 — now consistent
- The old `run.sh` (mahimahi-based) is preserved for Linux testing
