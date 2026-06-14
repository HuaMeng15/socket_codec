#!/usr/bin/env bash
# One-tap build: compiles x264 from source (if needed) then builds the main project.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
X264_DIR="$PROJECT_ROOT/third_party/x264"

echo "=== Building x264 from source ==="
cd "$X264_DIR"

# Only reconfigure if config.mak is missing (fresh checkout)
if [ ! -f config.mak ]; then
  ./configure --enable-static --disable-cli --enable-pic
fi

make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" libx264.a
echo "x264 built: $X264_DIR/libx264.a"

echo ""
echo "=== Building socket_codec ==="
cd "$PROJECT_ROOT"
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

echo ""
echo "=== Build complete: build/socket_codec ==="
