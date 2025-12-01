# Socket Codec

A consistent low latency Real Time Communication (RTC) application that encodes YUV video frames using VVC (Versatile Video Coding) and transmits them over UDP sockets.

## Prerequisites

### 1. Install VVenc (VVC Encoder)

VVenc must be installed and available on your system before building socket_codec.

**Installation Steps:**

1. Clone the VVenc repository:
   ```bash
   git clone https://github.com/fraunhoferhhi/vvenc.git
   cd vvenc
   ```

2. Build and install VVenc:
   ```bash
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   make -j$(nproc)
   sudo make install
   ```

3. Verify installation:
   ```bash
   # Check if libraries are installed
   ls /usr/local/lib/libvvenc*
   ls /usr/local/include/vvenc/
   ```

### 2. Install VVdec (VVC Decoder)

VVdec must also be installed for the decoder functionality.

**Installation Steps:**

1. Clone the VVdec repository:
   ```bash
   git clone https://github.com/HuaMeng15/vvdec.git
   cd vvdec
   ```

2. Build and install VVdec:
   ```bash
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   make -j$(nproc)
   make install
   ```

3. Copy the library and headers to socket_codec:
   ```bash
   # From the vvdec directory, copy the library
   cp vvdec/lib/release-static/libvvdec.a /path/to/socket_codec/lib/.

   # Copy the header files
   cp -r vvdec/install/include/vvdec/* /path/to/socket_codec/include/vvdec/
   ```

### 3. System Requirements

- C++23 compatible compiler (GCC 11+ or Clang 14+)
- Make
- CMake (for building vvenc/vvdec)
- Linux or macOS

## Building Socket Codec

1. **Clone or navigate to the socket_codec directory:**
   ```bash
   cd socket_codec
   ```
2. **Build the project:**
   ```bash
   make
   ```


The executable will be created at `build/socket_codec`.

## Usage

### Sender Mode

The sender encodes YUV video frames and transmits them over UDP to a receiver.

**Basic Usage:**
```bash
./build/socket_codec --ip=<receiver_ip> --port=<receiver_port> \
  --input_video_file=<input.yuv> --width=<width> --height=<height> \
  --fps=<fps> --frames_to_encode=<num_frames>
```

**Example:**
```bash
# This will automaticly send input/Lecture_5s.yuv to 127.0.0.1
./build/socket_codec
```

### Receiver Mode

The receiver listens for UDP packets, reassembles frames, decodes them, and saves the encoded bitstream to a file.

**Basic Usage:**
```bash
./build/socket_codec --file=<output_file> --port=<listen_port> \
  --width=<width> --height=<height>
```

**Example:**
```bash
./build/socket_codec --file=result/rec.y4m
```

### Command Line Arguments

| Argument | Type | Default | Description |
|----------|------|---------|-------------|
| `--file` | string | `"NONE"` | Output file path for receiver mode. Use `"NONE"` for sender mode. |
| `--ip` | string | `"127.0.0.1"` | Receiver IP address (sender mode only) |
| `--port` | int | `8888` | UDP port number |
| `--width` | int | `1920` | Video width in pixels |
| `--height` | int | `1080` | Video height in pixels |
| `--fps` | int | `30` | Frame rate (sender mode only) |
| `--frames_to_encode` | int | `10` | Number of frames to encode (0 or negative = encode all frames) |
| `--input_video_file` | string | `"input/Lecture_5s.yuv"` | Input YUV file path (sender mode only) |
| `--output_video_file` | string | `"result/output.266"` | Output encoded file path (sender mode, for local saving) |
| `--help` | flag | - | Show help message |

## Network Configuration

### Firewall Settings (Linux)

If you're running the receiver on Linux, ensure UDP port is open:

```bash
sudo ufw allow 8888/udp
sudo ufw reload
```

## File Structure

```
socket_codec/
├── build/              # Build output directory
├── codec/              # Encoder and decoder implementation
├── config/             # Configuration and command-line parsing
├── input/              # Input YUV video files
├── lib/                # Static libraries (libvvenc.a, libvvdec.a)
├── log_system/         # Logging system
├── result/             # Output files (encoded bitstreams, decoded YUV)
├── transmission/       # UDP packet transmission (sender/receiver)
├── tools/              # Utility tools
├── Makefile            # Build configuration
└── README.md           # This file
```
