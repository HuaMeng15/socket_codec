# Congestion Control Pipeline — Architecture

## System Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              SENDER                                          │
│                                                                             │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐                  │
│  │ FrameCapture │───▶│   Encoder    │───▶│  DataSender  │──┐               │
│  └──────────────┘    └──────────────┘    └──────────────┘  │               │
│         ▲                    ▲                   │          │               │
│         │                    │                   │          ▼               │
│  ┌──────────────┐            │            ┌─────────────────────┐          │
│  │  ClockThread │            │            │   NetworkSender     │          │
│  │  (frame tick)│            │            │  ┌───────────────┐  │          │
│  └──────────────┘            │            │  │NetworkSimulator│  │          │
│                              │            │  │(optional)      │  │          │
│                              │            │  └───────────────┘  │          │
│     ┌────────────────────────┼────────────┘         │           │          │
│     │  SetTargetBitrate()    │                      │           │          │
│     │                        │                      ▼           │          │
│     │                 SetTargetBitrate()        UDP socket       │          │
│     │                        │                      │           │          │
│  ┌──┴───┐              ┌─────┴─────┐               │           │          │
│  │Pacer │              │    GCC    │               │    PacketsSent(N)     │
│  └──────┘              │ Controller│◀──────────────┘─ ─ ─ ─ ─ ─┘          │
│                        └───────────┘                                        │
│                         ▲    ▲    ▲                                          │
│          ┌──────────────┘    │    └──────────────┐                          │
│          │                   │                   │                          │
│  OnTransportFeedback   OnLossReport      OnPacketsSent                     │
│          │                   │                   │                          │
│  ┌───────┴───────────────────┴───────────────────┘                          │
│  │             FeedbackHandler                                              │
│  │  (parses incoming feedback messages from receiver)                       │
│  └──────────────────────────────────────────────────┐                       │
│                                                     │                       │
│                                              DataReceiver                   │
│                                            (feedback port)                  │
└─────────────────────────────────────────────────────┼───────────────────────┘
                                                      │
                              ┌────────────────────────┼────────────────────┐
                              │           UDP          │                    │
                              │                        ▼                    │
┌─────────────────────────────┼────────────────────────────────────────────┐
│                              │         RECEIVER                           │
│                              ▼                                            │
│                       ┌─────────────┐                                    │
│                       │DataReceiver │                                    │
│                       │ (data port) │                                    │
│                       └──────┬──────┘                                    │
│                              │                                           │
│                              ▼                                           │
│                 ┌────────────────────────┐                               │
│                 │ReceivedFrameDataHandler│                               │
│                 │  • Reassemble frames   │                               │
│                 │  • Decode              │                               │
│                 └────────────┬───────────┘                               │
│                              │                                           │
│                              ▼                                           │
│                   ┌───────────────────┐      ┌─────────────────┐        │
│                   │FeedbackCollector  │─────▶│  DataSender     │        │
│                   │• Per-packet arrival│      │(feedback port)  │        │
│                   │  timestamps       │      └─────────────────┘        │
│                   │• Loss detection   │              │                   │
│                   │• Batched TWCC send │              │                   │
│                   └───────────────────┘              ▼                   │
│                                                 UDP to Sender            │
└──────────────────────────────────────────────────────────────────────────┘
```

## GCC Controller Internal Structure

```
┌─────────────────────────────────────────────────────────────────┐
│                      GccController                               │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │              Delay-Based Component                        │    │
│  │                                                          │    │
│  │  TransportFeedback ──▶ ┌────────────────────┐           │    │
│  │                        │ Trendline Estimator │           │    │
│  │                        │ • recv_delta -      │           │    │
│  │                        │ • Accumulated delay │           │    │
│  │                        │ • Linear regression │           │    │
│  │                        │   (≤100ms window)  │           │    │
│  │                        └─────────┬──────────┘           │    │
│  │                                  │ slope                 │    │
│  │                                  ▼                       │    │
│  │                        ┌────────────────────┐           │    │
│  │                        │  Overuse Detector  │           │    │
│  │                        │ • slope × gain(4.0)│           │    │
│  │                        │   vs threshold     │           │    │
│  │                        │ • Adaptive threshold│           │    │
│  │                        │   [6ms, 600ms]     │           │    │
│  │                        │ • Counter > 1        │           │    │
│  │                        └─────────┬──────────┘           │    │
│  │                                  │ Overuse/Normal/      │    │
│  │                                  │ Underuse             │    │
│  │                                  ▼                       │    │
│  │                        ┌────────────────────┐           │    │
│  │                        │   AIMD Rate Ctrl   │           │    │
│  │                        │ • Overuse:0.85×ack │           │    │
│  │                        │ • Normal:+8%/s,cap │           │    │
│  │                        │ • 1s cooldown      │           │    │
│  │                        └─────────┬──────────┘           │    │
│  │                                  │ delay_based_bitrate  │    │
│  └──────────────────────────────────┼──────────────────────┘    │
│                                     │                            │
│  ┌──────────────────────────────────┼──────────────────────┐    │
│  │              Loss-Based Component│                        │    │
│  │                                  │                        │    │
│  │  LossReport + OnPacketsSent ──▶ loss_fraction            │    │
│  │                                  │                        │    │
│  │  • < 2%  ──▶ increase (+8%/s)   │                        │    │
│  │  • 2-10% ──▶ hold               │                        │    │
│  │  • ≥ 10% ──▶ × (1 - 0.5×loss)  │                        │    │
│  │                                  │ loss_based_bitrate    │    │
│  └──────────────────────────────────┼──────────────────────┘    │
│                                     │                            │
│  ┌──────────────────────────────────┼──────────────────────┐    │
│  │           Bandwidth Prober       │                        │    │
│  │                                  │                        │    │
│  │  Triggers:                       │                        │    │
│  │  1. Startup: 3×, 6× exponential │                        │    │
│  │  2. ALR: 1.5× every 5s          │                        │    │
│  │  3. Drop: 0.85×pre-drop,        │                        │    │
│  │     only if queue draining      │                        │    │
│  │                                  │                        │    │
│  │  State: Idle→Probing→Waiting     │ probe_bitrate         │    │
│  └──────────────────────────────────┼──────────────────────┘    │
│                                     │                            │
│                                     ▼                            │
│                          ┌──────────────────┐                    │
│                          │  Final Bitrate   │                    │
│                          │                  │                    │
│                          │ base = min(delay,│                    │
│                          │         loss)    │                    │
│                          │                  │                    │
│                          │ if probing:      │                    │
│                          │   max(base,probe)│                    │
│                          │                  │                    │
│                          │ clamp [min, max] │                    │
│                          └────────┬─────────┘                    │
│                                   │                              │
│                                   ▼                              │
│                          target_bitrate_kbps                     │
└──────────────────────────────────────────────────────────────────┘
```

## Component Descriptions

### Sender Side

| Component | File | Role |
|-----------|------|------|
| **ClockThread** | `tools/clock_thread.h` | Single timing source. Provides frame ticks and slice deadlines for the entire pipeline. |
| **Encoder** | `codec/encoder.h` | Encodes video frames. `SetTargetBitrate()` adjusts quality in response to CC decisions. |
| **DataSender** | `transmission/data_sender.h` | Splits encoded frames into UDP packets. Fires `PacketsSentCallback` after each frame for loss accounting. |
| **NetworkSender** | `transmission/network_sender.h` | Wraps `send()` with optional `NetworkSimulator`. Transparent when simulator is off. |
| **NetworkSimulator** | `transmission/network_simulator.h` | Token-bucket bandwidth, propagation delay, random loss. Replaces mahimahi for macOS dev. |
| **Pacer** | `transmission/pacer.h` | Spreads packets over time at the target bitrate to avoid bursts. |
| **FeedbackHandler** | `transmission/feedback_handler.h` | Parses incoming feedback (TWCC batches, loss reports, legacy ACKs). Dispatches via callbacks to GCC. |
| **GccController** | `transmission/gcc_controller.h` | Core congestion control. Combines delay-based and loss-based estimates to produce target bitrate. |
| **BandwidthProber** | `transmission/bandwidth_prober.h` | Probes for available headroom. Temporarily overrides target bitrate during probe. |

### Receiver Side

| Component | File | Role |
|-----------|------|------|
| **DataReceiver** | `transmission/data_receiver.h` | Listens on UDP, dispatches packets to handler. |
| **ReceivedFrameDataHandler** | `transmission/received_frame_data_handler.h` | Reassembles packets into frames, decodes, writes output. |
| **FeedbackCollector** | `transmission/feedback_collector.h` | Collects per-packet arrival timestamps. Sends batched TWCC feedback back to sender every N packets. Detects loss via received_mask gaps. |

## Packet Formats

All multi-byte fields are **network byte order (big-endian)**. The default MTU
budget is `kDefaultMaxPacketSize = 1460` bytes (`config/config.h`), so each data
packet carries up to `1460 − 6 = 1454` payload bytes — the same number the
acked-throughput estimator assumes per packet.

### Sender → Receiver: data packet (data port)

Each encoded frame is split into N packets (`N = ceil(nal_size / 1454)`), each
prefixed with a 6-byte `FramePacketHeader` (`transmission/packet_header.h`):

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
┌───────────────────────────────┬───────────────┬───────────────┐
│      frame_sequence (u16)     │ packet_index  │ total_packets │
│                               │     (u8)      │     (u8)      │
├───────────────────────────────┴───────────────┴───────────────┤
│      payload_size (u16)       │                               │
├───────────────────────────────┘                               │
│              payload  (payload_size bytes, ≤ 1454)             │
│                            ...                                 │
└────────────────────────────────────────────────────────────────┘
```

| Field | Type | Meaning |
|-------|------|---------|
| `frame_sequence` | u16 | Frame this packet belongs to. |
| `packet_index` | u8 | 0-based index of this packet within the frame. |
| `total_packets` | u8 | Total packets the frame was split into (for reassembly). |
| `payload_size` | u16 | Bytes of encoded data in this packet (≤ 1454). |
| `payload` | bytes | The encoded H.264 slice fragment. |

Header size: **6 bytes**. The sender records each
`(frame_sequence, packet_index)` send time in `PacketSendTimeStore` at transmit
time — this is later joined with the receiver's arrival timestamp to form the
trendline deltas.

### Receiver → Sender: feedback packets (feedback port)

Feedback messages share a 8-byte `FeedbackMessageHeader`
(`transmission/transport_feedback.h`), followed by a variable number of records
whose type depends on `message_type`:

```
┌────────────────────────────────────────────────────────────────┐
│ FeedbackMessageHeader (8 bytes)                                │
│  message_type (u8)  reserved (u8)  record_count (u16)          │
│  sender_ssrc (u32, currently unused)                          │
└────────────────────────────────────────────────────────────────┘
   message_type: 0 = LegacyAck, 1 = TransportFeedback (TWCC), 2 = LossReport
```

**TransportFeedback (message_type = 1)** — `record_count` × `PacketArrivalRecord`
(8 bytes each):

```
┌───────────────────────────────┬───────────────┬───────────────┐
│      frame_sequence (u16)     │ packet_index  │  padding (u8) │
│                               │     (u8)      │               │
├───────────────────────────────┴───────────────┴───────────────┤
│             arrival_time_us (i32, μs since epoch)             │
└────────────────────────────────────────────────────────────────┘
```

`arrival_time_us` is relative to the receiver's **persistent epoch** (first packet
ever received, not per-batch) so inter-packet deltas stay consistent across batch
boundaries. **Microsecond** resolution — millisecond rounding injected
quantization noise into the trendline at high bitrates. Batches are sent every
`feedback_interval_` = 20 packets.

**LossReport (message_type = 2)** — `record_count` × `PacketLossRecord`
(4 bytes each):

```
┌───────────────────────────────┬───────────────┬───────────────┐
│      frame_sequence (u16)     │ packet_index  │  padding (u8) │
└───────────────────────────────┴───────────────┴───────────────┘
```

One record per packet the receiver detected as lost (sequence-gap /
eviction-based detection, `kFrameLookback = 10`).

**LegacyAck (message_type = 0)** — older per-packet ACK using the 4-byte
`FeedbackPacketHeader` (`frame_sequence` u16, `packet_index` u8, `feedback_type`
u8). Kept for backward compatibility; the GCC path uses TWCC + LossReport.

> Disambiguation: `FeedbackHandler` distinguishes new-style messages from legacy
> ACKs by inspecting `message_type` (the first byte) — see
> `feedback_handler.cc`.

## Algorithm Mechanics

This section explains, step by step, exactly how each estimate is computed in
`gcc_controller.cc`. It answers: how the delay trend is calculated, when and how
the delay-based rate moves, how the loss-based rate moves, what the lines in the
result plot mean, and how probing decides when to fire and at what rate.

### 1. Delay trend — how it is computed

Source: `UpdateTrendline()` + `ComputeTrendlineSlope()`, faithful to WebRTC
`trendline_estimator.cc`. The receiver timestamps every packet's arrival (TWCC),
and the sender knows each packet's send time, so for every acknowledged packet we
form a **one-way delay variation**:

```
recv_delta = arrival_time(i) − arrival_time(i−1)     // gap on the receiver clock
send_delta = send_time(i)   − send_time(i−1)         // gap on the sender clock
delta      = recv_delta − send_delta                 // queuing-delay change (ms)
```

`delta > 0` means packet `i` took longer to traverse than `i−1` did — a queue is
building. The deltas are integrated and EWMA-smoothed:

```
accumulated_delay += delta
smoothed_delay = 0.9 · smoothed_delay + 0.1 · accumulated_delay
```

Each `(arrival_time, smoothed_delay)` point is pushed into a **duration-based
window** holding the most recent ≤ `kTrendlineWindowMs` (100 ms) of arrival span.
When older points are trimmed off the front, we refit a **centered
least-squares line** through the window; its slope is the `trend` — the rate at
which queuing delay is growing (positive) or draining (negative). A flat link
gives slope ≈ 0.

> Note: `accumulated_delay` is a *delay-variation integral*, not an absolute
> latency. It can go negative (queue draining) and is reset at probe start. The
> real per-frame latency in the plot is computed separately from logs
> (`recv_decode_time − send_time`), not from this signal.

### 2. When to reduce the delay-based rate (overuse detection)

Source: `Detect()`, faithful to `TrendlineEstimator::Detect`. The raw slope is
scaled into a comparable signal:

```
modified_trend = min(num_deltas, 60) · trend · 4.0     // gain = kThresholdGain
```

The early `min(num_deltas, 60)` factor is a **startup warm-up**: with few samples
the signal is damped so noise can't trigger a false overuse. We then compare
against the adaptive threshold and require *sustained* over-use before declaring
`kOveruse`:

```
if modified_trend >  +threshold:   accumulate time_over_using; overuse_counter++
   → declare OVERUSE only if  time_over_using > 10ms
                          AND  overuse_counter > 1
                          AND  trend >= prev_trend      (still worsening)
elif modified_trend < −threshold:  → UNDERUSE   (queue draining)
else:                              → NORMAL
```

All three conditions guard against reacting to a single noisy spike. On
`kOveruse` the counter and timer reset.

**Adaptive threshold** (`UpdateAdaptiveThreshold`): the threshold itself moves so
the detector stays sensitive on quiet links and tolerant on jittery ones:

```
k = (|modified_trend| < threshold) ? k_down(0.039) : k_up(0.0087)
threshold += k · (|modified_trend| − threshold) · dt        // dt capped at 100ms
threshold  = clamp(threshold, 6ms, 600ms)
```

It grows while over threshold, decays toward 6 ms otherwise. A spike guard skips
adaptation when `|modified_trend| > threshold + 15ms` (e.g. a sudden capacity
cliff) so a one-off jump doesn't desensitize the detector.

### 3. How the delay-based rate moves (AIMD)

Source: `UpdateDelayBasedRate()`, WebRTC `AimdRateControl`. It also relies on the
**acknowledged throughput** `acked_bitrate_kbps_` (`UpdateAckedBitrate`), which is
the rate that actually got through in the last feedback batch
(`(N−1)·payload·8 / arrival_span`, EWMA-smoothed at 0.95).

- **On OVERUSE — multiplicative decrease.** Snap *down* to 0.85 × the *measured*
  throughput, not 0.85 × the current estimate:

  ```
  delay_based = min(delay_based, 0.85 · acked_bitrate)
  ```

  Snapping to measured throughput drains the self-induced queue and lands the
  estimate right at ~capacity, instead of just shaving a possibly-inflated
  number. It never raises the rate on overuse.

- **On NORMAL / UNDERUSE — additive increase.** Grow ~8 % of the current rate per
  second (with a small absolute floor), but **capped at 1.5 × acked + 10 kbps**:

  ```
  new = delay_based + delay_based · 0.08 · seconds_elapsed
  new = min(new, 1.5 · acked_bitrate + 10)      // the runaway brake
  ```

  Because `acked` plateaus at the link capacity, this cap is what stops the
  estimate from ramping far past a saturated link. Increase is also suppressed
  for ~1 s after an overuse event (`last_overuse_time_ms` cooldown).

### 4. How the loss-based rate moves

Source: `UpdateLossBasedRate()` + `MaybeUpdateLossRate()`, WebRTC
`send_side_bandwidth_estimation.cc`. The loss fraction is recomputed on a timer
(every 500 ms, once ≥ 20 packets were sent) as `lost / sent` over the window:

```
loss < 2%      → loss_based = delay_based          (track; no loss = no cap below)
2% ≤ loss < 10% → hold (unchanged)
loss ≥ 10%     → loss_based ·= (1 − 0.5 · loss)    (floor 0.5×, i.e. ≤50% cut)
```

The key design choice (matching WebRTC's `GetUpperLimit`, where the loss-based
target is capped at the delay-based estimate): **with no meaningful loss the
loss-based estimate simply equals the delay-based estimate**, so the two coincide
and loss only ever pulls the target *below* delay-based during real loss. In our
SparkRTC reference run the two were identical ~99.7 % of the time.

The driver: re-evaluating on a timer (not only when a loss report arrives) lets
the loss-based estimate climb back up during clean periods — otherwise it would
stay frozen at its last reduced value and cap the target forever.

### 5. Combining the estimates → target

Source: `ComputeFinalBitrate()`:

```
base   = min(delay_based, loss_based)
target = (probing && probe_rate > base) ? probe_rate : base
target = clamp(target, min_bitrate, max_bitrate)
```

The slower/safer of the two estimates wins, except while a probe is actively
raising the rate to test for headroom.

### 6. Reading the result plot

`scripts/plot_gcc.py` produces two stacked panels from the run logs.

**Panel 1 — bitrate (kbps):**

| Line | Source | Meaning |
|------|--------|---------|
| **Link capacity** | schedule (`[SCHEDULE]` lines) | Ground-truth bottleneck the simulator enforces. Step function over time. |
| **GCC target** | `[GCC_STATE] target=` | The controller's final decision — what the encoder/pacer is told to use. This is `ComputeFinalBitrate()`. |
| **Delay-based** | `[GCC_STATE] delay_based=` | The AIMD delay estimate alone (§3). |
| **Loss-based** | `[GCC_STATE] loss_based=` | The loss estimate alone (§4). Sits on top of delay-based when loss is low. |
| **Achieved send rate** | `[DataSender] Sending frame` bytes, binned ~0.5 s | The rate actually put on the wire. Compared against *GCC target* it shows how well the encoder tracked the command, and against *link capacity* it shows utilization. |

A healthy run has GCC target tracking just under link capacity and achieved send
rate hugging the target. Target above capacity with rising latency = overshoot
(queue filling before overuse fires).

**Panel 2 — real one-way frame latency (ms):** `recv_decode_time − send_time` per
frame, taken straight from the send/recv logs (always ≥ 0). Red vertical marks
denote overuse events. This is the genuine end-to-end delay, distinct from the
trendline's internal delay-variation signal.

### 7. Bandwidth probing

Source: `BandwidthProber` + the probe-lifecycle block in `OnTransportFeedback()`.
Probing deliberately raises the send rate above the current estimate to discover
spare capacity quickly, instead of waiting for slow +8 %/s additive growth.

**When a probe triggers** (`MaybeInitiateProbe`, gated by ≥ 1 s since the last
overuse):

1. **Startup exponential** — while initial probing isn't done, probe at **3×**
   then **6×** the current estimate. This is how we ramp fast from the 500 kbps
   start toward link capacity.
2. **Drop recovery** — if the estimate falls below **0.66×** its prior value
   (capacity cliff), probe at **0.85× the pre-drop rate** to re-find the level
   quickly — **but only if the queue is draining** (an underuse signal within
   the last 3 s). A bitrate drop is itself a congestion symptom; probing back up
   while the link is still congested would deepen it. This mirrors WebRTC's
   `in_alr / alr_ended_recently` gate in `ProbeController::RequestProbe`. The
   drop flag is held (not discarded) until the link drains or the 5 s window
   expires.
3. **ALR (application-limited)** — if the app is sending below the estimate, probe
   at **1.5×** at most once per **5 s**.

**How the probe rate is chosen:** the multipliers above set the *target*
(`3×/6×/1.5×/0.85×`), always clamped to `max_bitrate`. No probe is launched if the
estimate is already ≥ 95 % of max, or if the computed target wouldn't exceed the
current estimate.

**How a probe resolves** (GCC side — WebRTC `ProbeBitrateEstimator` semantics):
WebRTC commits a probe the *moment* it has a received-rate sample for the probe
traffic — it does **not** wait a fixed time window. We mirror that exactly. When
a probe starts we snapshot a floor and reset the delay integrator; then on the
**first feedback batch that carries the probe traffic** we resolve it:

- **Abort (failure)** if the elevated rate already induced congestion: overuse
  fired, the queue grew past 80 ms (`kProbeAbortDelayMs`), or the loss estimate
  collapsed below half the floor.
- **Commit immediately** from the batch's *measured received rate*
  (`last_received_rate_kbps_`, the raw per-batch rate — not the EWMA-smoothed
  `acked`). The received rate is **self-limiting**: if the probe target exceeds
  capacity, those packets queue at the bottleneck and arrive at ~capacity, so we
  measure capacity directly. Following WebRTC's `kTargetUtilizationFraction`:
  - if `received < probe_target` (link saturated) → commit `0.95 × received`;
  - else → commit the full `probe_target`.

  On a real gain (`committed > delay_based`) we set `delay_based = committed` and
  reset AIMD pacing from the new base. This commits to *measured capacity*, never
  the over-target send rate — which is what avoids overshoot and converges fast
  (in the 10 Mbps run the probe chain reaches ~9.5 Mbps in ~1.5 s, then holds).

**Chaining:** a probe whose measured result is ≥ 0.7 × its target (and below
95 % of max) triggers a **further probe at 2×** the measured rate, so capacity is
found in a few exponential steps. Any overuse cancels an in-flight probe
immediately.

## Data Flow

The mechanics of each estimate are in **Algorithm Mechanics** above; this is the
end-to-end packet/feedback flow that drives them.

### Normal Operation (no congestion)

1. **Sender** encodes frame → splits into packets → `DataSender.SendFrame()`
2. `DataSender` fires `PacketsSentCallback(N)` → `gcc.OnPacketsSent(N)`
3. **Receiver** collects packets → `FeedbackCollector.OnPacketReceived()`
4. After 20 packets, collector sends **TransportFeedback** batch back to sender
5. Sender's `FeedbackHandler` parses it → calls `gcc.OnTransportFeedback()`
6. GCC trendline sees flat delay → `kNormal` → additive increase (+8%/s)
7. Prober may trigger exponential probe at startup (3×, 6×)
8. `target_bitrate` gradually increases → encoder/pacer updated

### Congestion Detected

1. Network fills up → packet arrival gaps grow (queuing delay)
2. Trendline slope goes positive → `modified_trend > threshold`
3. Sustained over threshold (>10ms, counter >1, still worsening) → `kOveruse`
4. AIMD: `delay_based_bitrate = min(current, 0.85 × acked_throughput)`
5. Prober cancelled if active
6. Encoder reduces quality, pacer slows down

### Packet Loss

1. Receiver detects missing packets (gap in sequence) → sends **LossReport**
2. Sender's `FeedbackHandler` dispatches → `gcc.OnLossReport()`
3. `loss_fraction = lost / sent` recomputed on the 500ms timer
4. If ≥ 10%: `loss_based_bitrate *= (1 - 0.5 × loss_fraction)`; if < 2%: tracks delay-based
5. `target = min(delay_based, loss_based)` → rate drops

### Bandwidth Probing

1. **Startup**: probes at 3× then 6× to discover capacity fast
2. **Drop recovery**: after a >66% bitrate drop, probes at 85% of pre-drop rate — only once the queue is draining (recent underuse)
3. **ALR**: when app sends below estimate, probes at 1.5× at most every 5s
4. Probe resolves on the first feedback batch carrying the probe traffic (no fixed window); commits the measured received rate (`0.95×` if the link is saturated)
5. On real gain (≥ 0.7 × target): commit and further-probe at 2×
6. On overuse / queue >80ms / loss collapse during probe: abort immediately

## Key Constants (WebRTC-aligned)

| Constant | Value | Source |
|----------|-------|--------|
| Trendline window | ≤100 ms (duration-based) | `trendline_estimator.cc` |
| Smoothing coeff | 0.9 | `trendline_estimator.cc` |
| Threshold gain | 4.0 | `trendline_estimator.cc` |
| modified_trend sample cap | min(num_deltas, 60) | `trendline_estimator.cc` |
| Threshold range | [6, 600] ms | `trendline_estimator.cc` |
| k_up (threshold growth) | 0.0087/ms | `trendline_estimator.cc` |
| k_down (threshold decay) | 0.039/ms | `trendline_estimator.cc` |
| Threshold spike guard | 15 ms | `trendline_estimator.cc` |
| Overusing-time threshold | 10 ms (counter >1) | overuse detector |
| Multiplicative decrease | 0.85 × acked | AIMD rate control |
| Additive increase | ~8%/s, cap 1.5×acked+10 | AIMD rate control |
| Acked-rate smoothing | 0.95 EWMA | AIMD rate control |
| Post-overuse cooldown | 1000 ms | rate control |
| Loss increase threshold | < 2% | `send_side_bandwidth_estimation.cc` |
| Loss hold range | 2–10% | `send_side_bandwidth_estimation.cc` |
| Loss decrease formula | 1 - 0.5×loss (floor 0.5) | `send_side_bandwidth_estimation.cc` |
| Loss re-eval interval | 500 ms | `send_side_bandwidth_estimation.cc` |
| Initial probe multipliers | 3×, 6× | `probe_controller.cc` |
| Further probe threshold | 0.7 | `probe_controller.cc` |
| Further probe multiplier | 2× | `probe_controller.cc` |
| ALR probe interval | 5000 ms | `probe_controller.cc` |
| ALR probe multiplier | 1.5× | `probe_controller.cc` |
| Drop threshold | 66% | `probe_controller.cc` |
| Drop recovery fraction | 0.85 | `probe_controller.cc` |
| Drop-recovery underuse window | 3000 ms | `probe_controller.cc` (in_alr gate) |
| Probe timeout | 3000 ms | `probe_controller.cc` |
| Min time between probes | 1000 ms | `probe_controller.cc` |
| Probe commit utilization | 0.95 × received (when saturated) | `probe_bitrate_estimator.cc` |
