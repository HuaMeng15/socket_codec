#ifndef CODEC_H264_SLICE_PACED_ENCODER_H
#define CODEC_H264_SLICE_PACED_ENCODER_H

#include <atomic>
#include <fstream>
#include <memory>

#include "../encoder.h"
#include "x264/x264.h"
#include "x264/x264_config.h"

class SlicePacedEncoder : public Encoder {
 public:
  SlicePacedEncoder();
  ~SlicePacedEncoder() override;

  int Initialize(int width, int height, int fps,
                 int framesToBeEncoded = -1) override;
  void SetOutputStream(std::ofstream* output_stream) override;
  std::unique_ptr<EncodedData> EncodeFrame(YUVBuffer* input_buffer) override;
  void Cleanup() override;
  void PrintSummary() const override;
  void SetTargetBitrate(int bitrate_kbps) override;
  void SetNetworkUsageState(double network_usage_state) override;

  bool SupportsSliceEncoding() const override { return true; }
  int GetSliceCount() const override { return slice_count_; }
  bool StartFrame(YUVBuffer* input_buffer) override;
  std::unique_ptr<EncodedData> EncodeSlice(int slice_idx) override;
  bool FinishFrame() override;

 private:
  void CopyInputToPicture(YUVBuffer* input_buffer);
  void ApplyBitrateReconfig(int bitrate_kbps);
  int SelectSliceCountForBitrate(int bitrate_kbps) const;
  void ApplyPendingSliceCount();
  bool ShouldUseSliceRateControl() const;

  x264_t* encoder_;
  x264_param_t params_;
  x264_picture_t pic_in_;
  x264_picture_t pic_out_;

  std::ofstream* output_stream_;
  bool initialized_;
  bool frame_in_progress_;
  int width_;
  int height_;
  int fps_;
  int slice_count_;
  uint16_t sequence_number_;

  std::atomic<int> target_bitrate_kbps_;
  std::atomic<int> pending_slice_count_;
  std::atomic<double> network_usage_state_;
  int frame_start_effective_bitrate_kbps_;
  bool slice_rc_active_;
};

#endif  // CODEC_H264_SLICE_PACED_ENCODER_H
