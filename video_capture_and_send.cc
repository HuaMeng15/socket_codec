#include "video_capture_and_send.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

#include "codec/codec_factory.h"
#include "log_system/log_system.h"
#include "transmission/pacer.h"

VideoCaptureAndSend::VideoCaptureAndSend()
    : stop_requested_(false),
      initialized_(false) {
}

VideoCaptureAndSend::~VideoCaptureAndSend() {
  Cleanup();
}

int VideoCaptureAndSend::Initialize(const std::string& input_file,
                                     const std::string& output_file,
                                     const std::string& dest_ip,
                                     int dest_port,
                                     int width,
                                     int height,
                                     int fps,
                                     int frames_to_encode,
                                     CodecType codec_type,
                                     EncoderRateControlMode rate_control_mode,
                                     const std::string& bind_interface) {
  if (initialized_) {
    LOG(WARNING) << "[VideoCaptureAndSend] Already initialized";
    return 0;
  }

  pacer_ = std::make_unique<Pacer>();
  data_sender_ = std::make_unique<DataSender>();
  data_sender_->SetBindInterface(bind_interface);
  if (0 != data_sender_->Initialize(dest_ip, dest_port)) {
    LOG(ERROR) << "[VideoCaptureAndSend] Failed to initialize data sender";
    return -1;
  }
  data_sender_->SetPacer(pacer_.get());
  if (send_time_store_) {
    data_sender_->SetSendTimeStore(send_time_store_);
  }
  if (simulator_) {
    data_sender_->SetSimulator(simulator_);
  }

  frame_capture_ = std::make_unique<FrameCapture>();
  if (0 != frame_capture_->Initialize(input_file, width, height, fps)) {
    LOG(ERROR) << "[VideoCaptureAndSend] Failed to initialize frame capture";
    return -1;
  }

  encoder_ = CodecFactory::CreateEncoder(codec_type);
  encoder_->SetRateControlMode(rate_control_mode);
  if (0 != encoder_->Initialize(width, height, fps, frames_to_encode)) {
    LOG(ERROR) << "[VideoCaptureAndSend] Failed to initialize encoder";
    return -1;
  }

  // Open output file for encoded data (for debugging)
  if (!output_file.empty()) {
    output_stream_.open(output_file, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output_stream_.is_open()) {
      LOG(ERROR) << "[VideoCaptureAndSend] Failed to open output file: " << output_file;
      return -1;
    }
    encoder_->SetOutputStream(&output_stream_);
  }

  fps_ = fps;
  max_frames_ = frames_to_encode;

  initialized_ = true;
  stop_requested_ = false;

  LOG(INFO) << "[VideoCaptureAndSend] Initialized successfully";
  return 0;
}

void VideoCaptureAndSend::Run() {
  if (!initialized_) {
    LOG(ERROR) << "[VideoCaptureAndSend] Not initialized";
    return;
  }

  LOG(INFO) << "[VideoCaptureAndSend] Starting capture and send loop";

  clock_.SetFps(fps_);
  clock_.Start();
  bool is_eof = false;
  int frames_sent = 0;

  while (!stop_requested_) {
    int frame_idx = clock_.WaitForNextFrameTick();
    if (frame_idx < 0) {
      break;  // clock stopped
    }

    is_eof = false;
    auto frame_buffer = frame_capture_->ReadNextFrame(is_eof);
    if (!frame_buffer) {
      if (is_eof && max_frames_ > 0) {
        frame_capture_->Reset();
        continue;
      }
      if (is_eof) {
        LOG(INFO) << "[VideoCaptureAndSend] Reached end of input";
        break;
      }
      LOG(ERROR) << "[VideoCaptureAndSend] Failed to read next frame";
      break;
    } else {
      clock_.MarkFrameReadComplete();
      const int64_t capture_time_us =
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      LOG(INFO) << "[VideoCaptureAndSend] Read frame "
                << frame_buffer->sequence_number
                << " capture_time_us=" << capture_time_us;
    }

    uint16_t sent_sequence = 0;
    bool frame_sent = false;

    if (encoder_->SupportsSliceEncoding()) {
      if (!encoder_->StartFrame(frame_buffer.get())) {
        LOG(ERROR) << "[VideoCaptureAndSend] Failed to start sliced frame";
        continue;
      }

      const int slice_count = encoder_->GetSliceCount();
      clock_.SetSliceCount(slice_count);
      uint16_t next_packet_index = 0;
      bool send_ok = true;

      for (int slice = 0; slice < slice_count; slice++) {
        const int64_t slice_deadline_us = clock_.GetSliceDeadline(frame_idx, slice);
        // GetSliceDeadline() is the time by which this slice should be
        // complete. Start it at the beginning of its allocation instead of
        // waiting until the completion deadline before encoding. This is
        // especially important when adaptive slicing selects one slice: the
        // single slice must start immediately, not one full frame interval
        // after capture.
        const int64_t slice_interval_us =
            clock_.GetFrameIntervalUs() / std::max(1, slice_count);
        const int64_t slice_start_us = slice_deadline_us - slice_interval_us;
        const int64_t now_us = clock_.GetCurrentTimeUs();
        if (slice_start_us > now_us) {
          std::this_thread::sleep_for(
              std::chrono::microseconds(slice_start_us - now_us));
        }
        auto slice_data = encoder_->EncodeSlice(slice);
        if (!slice_data) {
          LOG(ERROR) << "[VideoCaptureAndSend] Failed to encode slice " << slice;
          send_ok = false;
          break;
        }
        if (slice == 0) {
          sent_sequence = slice_data->sequence_number;
        }

        size_t fragment_packets = data_sender_->CountFramePackets(slice_data.get());
        if (fragment_packets == 0 ||
            static_cast<size_t>(next_packet_index) + fragment_packets >
                std::numeric_limits<uint16_t>::max()) {
          LOG(ERROR) << "[VideoCaptureAndSend] Invalid sliced packet count";
          send_ok = false;
          break;
        }

        bool is_final_slice = (slice == slice_count - 1);
        uint16_t total_packets_for_header = 0;
        if (is_final_slice) {
          total_packets_for_header =
              static_cast<uint16_t>(next_packet_index + fragment_packets);
        }

        uint16_t packets_sent = 0;
        int ret = data_sender_->SendFrameFragment(slice_data.get(),
                                                  next_packet_index,
                                                  total_packets_for_header,
                                                  &packets_sent);
        if (ret != 0) {
          LOG(ERROR) << "[VideoCaptureAndSend] Failed to send slice " << slice;
          send_ok = false;
          break;
        }
        next_packet_index += packets_sent;
      }

      if (!encoder_->FinishFrame()) {
        LOG(ERROR) << "[VideoCaptureAndSend] Failed to finish sliced frame";
        send_ok = false;
      }

      if (!send_ok) {
        continue;
      }
      frame_sent = true;
      LOG(INFO) << "[VideoCaptureAndSend] Successfully sent sliced frame "
                << sent_sequence;
    } else {
      auto encoded_data = encoder_->EncodeFrame(frame_buffer.get());

      if (!encoded_data) {
        LOG(ERROR) << "[VideoCaptureAndSend] Failed to encode frame";
        continue;
      }

      sent_sequence = encoded_data->sequence_number;

      // Send encoded data via network
      if (encoded_data->size > 0) {
        int ret = data_sender_->SendFrame(encoded_data.get());
        if (ret != 0) {
          LOG(ERROR) << "[VideoCaptureAndSend] Failed to send encoded data";
        } else {
          frame_sent = true;
          LOG(INFO) << "[VideoCaptureAndSend] Successfully sent encoded data for frame " << encoded_data->sequence_number;
        }
      }
    }

    if (!frame_sent) {
      continue;
    }

    frames_sent++;

    if (max_frames_ > 0 && frames_sent >= max_frames_) {
      LOG(INFO) << "[VideoCaptureAndSend] Max frames limit reached: " << max_frames_;
      break;
    }
  }

  clock_.Stop();
  LOG(INFO) << "[VideoCaptureAndSend] Capture and send loop finished.";
}

void VideoCaptureAndSend::Stop() {
  stop_requested_ = true;
}

bool VideoCaptureAndSend::IsStopped() const {
  return stop_requested_.load();
}

void VideoCaptureAndSend::Cleanup() {
  if (!initialized_) {
    return;
  }

  Stop();

  if (encoder_) {
    encoder_->Cleanup();
    encoder_.reset();
  }

  if (frame_capture_) {
    frame_capture_.reset();
  }

  if (data_sender_) {
    data_sender_->Close();
    data_sender_.reset();
  }

  if (output_stream_.is_open()) {
    output_stream_.close();
  }

  initialized_ = false;
  LOG(INFO) << "[VideoCaptureAndSend] Cleaned up";
}

void VideoCaptureAndSend::PrintSummary() const {
  if (encoder_) {
    encoder_->PrintSummary();
  }
}
