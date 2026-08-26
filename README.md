# Socket Codec

Socket Codec is a low-latency RTC experiment that encodes YUV video frames,
transmits them over UDP, and decodes the received stream. The default build is
focused on H.264/x264:

- `x264`: frame-level x264 encoder
- `x264_slice`: slice-level x264 encoder
- `mock`: test codec
- `vvenc`: optional VVenC/VVdec path, enabled with `make VVENC=1`

## Requirements

- C++23-capable compiler (`g++` or `clang++`)
- `make`
- `git`
- `pkg-config` is useful, but not required for the main binary
- `python3`, `numpy`, and `matplotlib` for analysis/plot scripts
- Optional: an `ffmpeg` command with `libvmaf` support for PSNR/VMAF compare
  scripts

On macOS, Homebrew's FFmpeg usually includes `libvmaf`:

```bash
brew install ffmpeg
/opt/homebrew/bin/ffmpeg -hide_banner -h filter=libvmaf
```

## Third-Party Layout

The project expects source builds under `third_party/`:

```text
third_party/
├── x264/      # x264 source checkout; builds third_party/x264/libx264.a
└── ffmpeg/    # FFmpeg source submodule; builds static libavcodec/libavutil
```

### Do We Need the FFmpeg Repo?

Yes, for the current H.264 build. `codec/h264/x264_decoder.cc` uses FFmpeg's
`libavcodec` and `libavutil`, and the Makefile links static libraries from:

```text
third_party/ffmpeg/build/libavcodec/libavcodec.a
third_party/ffmpeg/build/libavutil/libavutil.a
```

This is separate from the `ffmpeg` command-line tool used by scripts. The repo
needs `third_party/ffmpeg` to build `socket_codec`; the compare scripts can use a
system FFmpeg binary through `FFMPEG_BIN`.

## Build From a Fresh Checkout

### 1. Initialize FFmpeg

`third_party/ffmpeg` is a submodule:

```bash
git submodule update --init --recursive third_party/ffmpeg
```

Build the static FFmpeg libraries needed by the decoder:

```bash
cd third_party/ffmpeg
mkdir -p build
cd build
../configure --disable-programs --disable-doc \
  --disable-avdevice --disable-avfilter \
  --disable-swscale --disable-swresample \
  --disable-videotoolbox --disable-audiotoolbox \
  --disable-iconv --disable-zlib
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
cd ../../..
```

After this step, these files should exist:

```bash
ls third_party/ffmpeg/build/libavcodec/libavcodec.a
ls third_party/ffmpeg/build/libavutil/libavutil.a
```

### 2. Set Up x264

`third_party/x264` is expected to be a source checkout. If it is missing, clone
it into that exact path:

```bash
git clone git@github.com:HuaMeng15/x264.git third_party/x264
```

If you do not have access to that fork, use upstream x264:

```bash
git clone https://code.videolan.org/videolan/x264.git third_party/x264
```

Then build x264 and the main project:

```bash
./scripts/build.sh
```

The script configures x264 if needed, builds `third_party/x264/libx264.a`, and
then runs `make` for the main binary.

### 3. Build Manually

Equivalent manual commands:

```bash
cd third_party/x264
./configure --enable-static --disable-cli --enable-pic
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" libx264.a

cd ../..
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
```

The executable is written to:

```text
build/socket_codec
```

## Optional VVenC/VVdec Build

The default build does not require VVenC or VVdec. To enable the optional VVC
codec path, build the static libraries and make the headers visible to the
compiler:

```text
lib/libvvenc.a
lib/libvvdec.a
vvenc/vvenc.h or an equivalent include path for vvenc/
vvdec/vvdec.h or an equivalent include path for vvdec/
```

Then build with:

```bash
make VVENC=1
```

## Running

Start a receiver:

```bash
./build/socket_codec --codec=x264 --file=result/rec.yuv --port=8888 \
  --width=1920 --height=1080
```

Start a sender:

```bash
./build/socket_codec --codec=x264 --ip=127.0.0.1 --port=8888 \
  --input_video_file=input/Lecture_5s.yuv \
  --width=1920 --height=1080 --fps=30 --frames_to_encode=150
```

For the slice-level encoder:

```bash
./build/socket_codec --codec=x264_slice ...
```

## Experiment Scripts

Single x264/x264_slice run:

```bash
CODEC=x264_slice ./scripts/run_x264_test.sh
```

Repeated comparison:

```bash
./scripts/run_x264_compare.sh
```

Useful overrides:

```bash
CODECS="x264 x264_slice" TRIALS=3 RUN_PLOTS=1 ./scripts/run_x264_compare.sh
FFMPEG_BIN=/opt/homebrew/bin/ffmpeg ./scripts/run_x264_compare.sh
```

`run_x264_compare.sh` writes per-trial logs, quality metrics, latency metrics,
and figures under `result/x264_compare_10to1_<timestamp>/`.

## Common Build Issues

- Missing `x264/x264.h`: clone/build `third_party/x264`.
- Missing `libx264.a`: run `./scripts/build.sh` or build x264 manually.
- Missing `libavcodec/avcodec.h`: initialize `third_party/ffmpeg`.
- Missing `libavcodec.a` or `libavutil.a`: build FFmpeg under
  `third_party/ffmpeg/build`.
- Missing VMAF in compare output: set `FFMPEG_BIN` to an FFmpeg binary that has
  `libvmaf`, for example `/opt/homebrew/bin/ffmpeg` on macOS.
