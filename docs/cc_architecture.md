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
│  │                        │ • Inter-group delta │           │    │
│  │                        │ • Accumulated delay │           │    │
│  │                        │ • Linear regression │           │    │
│  │                        │   (window=20)       │           │    │
│  │                        └─────────┬──────────┘           │    │
│  │                                  │ slope                 │    │
│  │                                  ▼                       │    │
│  │                        ┌────────────────────┐           │    │
│  │                        │  Overuse Detector  │           │    │
│  │                        │ • slope × gain(4.0)│           │    │
│  │                        │   vs threshold     │           │    │
│  │                        │ • Adaptive threshold│           │    │
│  │                        │   [6ms, 600ms]     │           │    │
│  │                        │ • Counter ≥ 3      │           │    │
│  │                        └─────────┬──────────┘           │    │
│  │                                  │ Overuse/Normal/      │    │
│  │                                  │ Underuse             │    │
│  │                                  ▼                       │    │
│  │                        ┌────────────────────┐           │    │
│  │                        │   AIMD Rate Ctrl   │           │    │
│  │                        │ • Overuse: ×0.85   │           │    │
│  │                        │ • Normal: +8%/s    │           │    │
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
│  │  3. Drop recovery: 0.85×pre-drop│                        │    │
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

### Feedback Wire Format

```
┌────────────────────────────────────────┐
│ FeedbackMessageHeader (4 bytes)        │
│  • message_type: 1=TWCC, 2=LossReport │
│  • record_count                        │
└────────────────────────────────────────┘
│
├─ If TWCC: N × PacketArrivalRecord (8 bytes each)
│   • frame_sequence, packet_index
│   • arrival_time_ms (relative to batch start)
│
└─ If LossReport: N × PacketLossRecord (4 bytes each)
    • frame_sequence, packet_index
```

## Data Flow

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
3. Overuse counter hits 3 → `kOveruse` signal
4. AIMD: `delay_based_bitrate *= 0.85`
5. Prober cancelled if active
6. Encoder reduces quality, pacer slows down

### Packet Loss

1. Receiver detects missing packets (gap in sequence) → sends **LossReport**
2. Sender's `FeedbackHandler` dispatches → `gcc.OnLossReport()`
3. `loss_fraction = lost / sent` computed over sliding window
4. If ≥ 10%: `loss_based_bitrate *= (1 - 0.5 × loss_fraction)`
5. `target = min(delay_based, loss_based)` → rate drops

### Bandwidth Probing

1. **Startup**: immediately probes at 3× and 6× to discover capacity fast
2. **ALR**: when app sends below estimate for 5s, probes at 1.5×
3. **Drop recovery**: after 66% bitrate drop, probes at 85% of pre-drop rate
4. Probe succeeds if feedback shows no overuse at elevated rate
5. On success (est > 0.7 × target): commit and optionally further-probe at 2×
6. On overuse during probe: cancel immediately, revert

## Key Constants (WebRTC-aligned)

| Constant | Value | Source |
|----------|-------|--------|
| Trendline window | 20 samples | `trendline_estimator.h` |
| Smoothing coeff | 0.9 | `trendline_estimator.cc` |
| Threshold gain | 4.0 | `trendline_estimator.cc` |
| Threshold range | [6, 600] ms | `trendline_estimator.cc` |
| k_up (threshold growth) | 0.0087/ms | `trendline_estimator.cc` |
| k_down (threshold decay) | 0.039/ms | `trendline_estimator.cc` |
| Overuse counter threshold | 3 | overuse detector |
| Multiplicative decrease | 0.85 | AIMD rate control |
| Additive increase | ~8%/s | AIMD rate control |
| Post-overuse cooldown | 1000 ms | rate control |
| Loss increase threshold | < 2% | `send_side_bandwidth_estimation.cc` |
| Loss hold range | 2–10% | `send_side_bandwidth_estimation.cc` |
| Loss decrease formula | 1 - 0.5×loss | `send_side_bandwidth_estimation.cc` |
| Initial probe multipliers | 3×, 6× | `probe_controller.cc` |
| Further probe threshold | 0.7 | `probe_controller.cc` |
| ALR probe interval | 5000 ms | `probe_controller.cc` |
| ALR probe multiplier | 1.5× | `probe_controller.cc` |
| Drop threshold | 66% | `probe_controller.cc` |
| Drop recovery fraction | 0.85 | `probe_controller.cc` |
| Probe timeout | 3000 ms | `probe_controller.cc` |
| Min time between probes | 1000 ms | `probe_controller.cc` |
