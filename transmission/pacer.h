#ifndef TRANSMISSION_PACER_H
#define TRANSMISSION_PACER_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/**
 * Pacer: a bounded token-bucket packet scheduler running on its own thread.
 *
 * Producers (the encode/send loop) call Enqueue() with fully-formed packets;
 * the pacer thread drains the queue at `multiplier × target` bitrate, releasing
 * a bounded burst (tokens capped at burst_cap_ms × rate) so a whole frame never
 * dumps at once. This matches WebRTC's PacedSender: the multiplier (default
 * 2.5) absorbs encoder bursts while the long-run average stays bounded by the
 * encoder's output.
 *
 * Probing: when SetProbing(true) is set and the real-packet queue drains, the
 * pacer emits padding packets (marked with kPaddingFrameSequence) up to the
 * probe rate. This fills the pipe so the probe's measured received rate
 * reflects true link capacity even before the encoder produces probe-rate
 * frames. During a probe the effective pace rate is 1.0× the target (the probe
 * target IS the send rate we want to test); outside probes it is `multiplier×`.
 *
 * Thread-safety: Enqueue()/SetTargetBitrate()/SetProbing() are callable from
 * any thread. Start() launches the drain thread; Stop() joins it.
 */
class Pacer {
 public:
  Pacer();
  ~Pacer();

  // send_fn actually transmits packet bytes (e.g. DataSender::SendPacket).
  // record_fn (optional) records a real packet's send time by identity; it is
  // NOT called for padding. Both are invoked from the pacer thread.
  using SendFn = std::function<void(const uint8_t* data, size_t size)>;
  using RecordFn = std::function<void(uint16_t frame_sequence, uint8_t packet_index)>;

  void SetSendCallback(SendFn fn) { send_fn_ = std::move(fn); }
  void SetRecordCallback(RecordFn fn) { record_fn_ = std::move(fn); }

  /** Pace at this multiple of target (bursts) outside probes. Default 2.5. */
  void SetPaceMultiplier(double m);
  /** Max burst allowed (in ms of data at the current rate). Default 5. */
  void SetBurstCapMs(double ms);
  /** Padding packet wire size (header + payload). Defaults to 1460. */
  void SetMaxPacketSize(size_t bytes) { max_packet_size_ = bytes; }

  /** Set target bitrate in kbps. During a probe this is the probe target. */
  void SetTargetBitrate(int bitrate_kbps);

  /** Toggle probing. While probing, idle time is filled with padding. */
  void SetProbing(bool probing);

  /** Enqueue a fully-formed real packet (header already written) for paced send. */
  void Enqueue(const uint8_t* data, size_t size,
               uint16_t frame_sequence, uint8_t packet_index);

  /** Launch the drain thread. Idempotent. */
  void Start();
  /** Stop and join the drain thread. Idempotent. */
  void Stop();

  /**
   * Returns total wire bytes sent since the last call (including padding),
   * then resets the counter to zero. Intended for the ALR detector: called
   * once per feedback batch so GccController can compute the send rate and
   * determine whether the sender is application-limited.
   */
  size_t ConsumeBytesSent();

 private:
  struct QueuedPacket {
    std::vector<uint8_t> data;
    uint16_t frame_sequence;
    uint8_t packet_index;
  };

  void Run();                 // drain-thread body
  double EffectiveRateBps();  // current pace rate in bits/s (probe-aware)
  void SendPadding(size_t payload_bytes);

  SendFn send_fn_;
  RecordFn record_fn_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<QueuedPacket> queue_;

  // Wire bytes sent since the last ConsumeBytesSent() (real + padding).
  size_t bytes_sent_since_consume_ = 0;

  int bitrate_kbps_;
  double pace_multiplier_ = 2.5;
  double burst_cap_ms_ = 5.0;
  size_t max_packet_size_ = 1460;
  bool probing_ = false;

  double tokens_bits_ = 0.0;  // available send credit, in bits
  std::chrono::steady_clock::time_point last_refill_;
  bool refill_initialized_ = false;
  uint32_t padding_counter_ = 0;

  std::thread thread_;
  std::atomic<bool> running_{false};

  // Safety valve: if producers outpace the drain (e.g. a real encoder
  // overshooting), cap the backlog and drop oldest packets rather than grow
  // unbounded. ~2s of a 20Mbps stream at 1460B packets ≈ 3400 packets.
  static constexpr size_t kMaxQueuePackets = 4000;
};

#endif  // TRANSMISSION_PACER_H
