#include "video_capture_and_send.h"

#include <chrono>

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
                                     CodecType codec_type) {
  if (initialized_) {
    LOG(WARNING) << "[VideoCaptureAndSend] Already initialized";
    return 0;
  }

  pacer_ = std::make_unique<Pacer>();
  data_sender_ = std::make_unique<DataSender>();
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
      LOG(INFO) << "[VideoCaptureAndSend] Read frame " << frame_buffer->sequence_number;
    }

    auto encoded_data = encoder_->EncodeFrame(frame_buffer.get());

    if (!encoded_data) {
      LOG(ERROR) << "[VideoCaptureAndSend] Failed to encode frame";
      continue;
    }

    // Send encoded data via network
    if (encoded_data->size > 0) {
      int ret = data_sender_->SendFrame(encoded_data.get());
      if (ret != 0) {
        LOG(ERROR) << "[VideoCaptureAndSend] Failed to send encoded data";
      } else {
        LOG(INFO) << "[VideoCaptureAndSend] Successfully sent encoded data for frame " << encoded_data->sequence_number;
      }
    }

    // Check if max frames reached (sequence_number is 0-based)
    if (max_frames_ > 0 &&
        static_cast<int>(encoded_data->sequence_number) + 1 >= max_frames_) {
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

