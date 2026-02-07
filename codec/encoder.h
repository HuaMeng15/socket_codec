#ifndef CODEC_ENCODER_H
#define CODEC_ENCODER_H

#include <atomic>
#include <fstream>
#include <memory>
#include <string>

// Forward declarations
class FrameCapture;
class DataSender;

// Common YUV buffer structure (codec-agnostic)
struct YUVBuffer {
  struct Plane {
    uint8_t* ptr;      // Pointer to plane data
    int stride;        // Stride in bytes
    int width;         // Width in pixels
    int height;        // Height in pixels
  };
  
  Plane planes[3];     // Y, U, V planes
  int64_t sequence_number;
  int64_t cts;         // Composition timestamp
  bool cts_valid;
  
  YUVBuffer() : sequence_number(0), cts(0), cts_valid(false) {
    for (int i = 0; i < 3; i++) {
      planes[i].ptr = nullptr;
      planes[i].stride = 0;
      planes[i].width = 0;
      planes[i].height = 0;
    }
  }

  YUVBuffer(int width, int height) : sequence_number(0), cts(0), cts_valid(false) {
    for (int i = 0; i < 3; i++) {
      // Allocate memory for the plane based on yuv420 format
      if (i == 0) {
        planes[i].ptr = new uint8_t[width * height];
        planes[i].stride = width;
        planes[i].width = width;
        planes[i].height = height;
      } else if (i == 1) {
        planes[i].ptr = new uint8_t[width * height / 4];
        planes[i].stride = width / 2;
        planes[i].width = width / 2;
        planes[i].height = height / 2;
      } else if (i == 2) {
        planes[i].ptr = new uint8_t[width * height / 4];
        planes[i].stride = width / 2;
        planes[i].width = width / 2;
        planes[i].height = height / 2;
      }
    }
  }

  ~YUVBuffer() {
    for (int i = 0; i < 3; i++) {
      if (planes[i].ptr) {
        delete[] planes[i].ptr;
        planes[i].ptr = nullptr;
      }
    }
  }
};

// Base encoded data structure
struct EncodedData {
  uint16_t sequence_number;
  size_t size;
  std::vector<uint8_t*> data_ptrs;
  std::vector<size_t> data_sizes;

  EncodedData() : sequence_number(0), size(0), data_ptrs() {}

  void AddData(uint8_t* data, size_t size) {
    data_ptrs.push_back(data);
    data_sizes.push_back(size);
    this->size += size;
  }

  ~EncodedData() {
    for (auto data : data_ptrs) {
      if (data) {
        delete[] data;
        data = nullptr;
      }
    }
    data_ptrs.clear();
    data_sizes.clear();
  }
};

class Encoder {
 public:
  Encoder() = default;
  virtual ~Encoder() = default;

  virtual int Initialize(int width, int height, int fps, int framesToBeEncoded = -1) = 0;

  virtual void SetOutputStream(std::ofstream* output_stream) = 0;

  virtual std::unique_ptr<EncodedData> EncodeFrame(YUVBuffer* input_buffer) = 0;

  virtual void Cleanup() = 0;

  virtual void PrintSummary() const = 0;
};

#endif  // CODEC_ENCODER_H

