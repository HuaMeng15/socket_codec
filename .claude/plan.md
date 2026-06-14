# Socket Codec — RTC Pipeline Plan

## Overview

Evolve the current UDP video pipeline into a research-grade RTC system with:
- Network-aware congestion control (GCC)
- Packet-level rate control (slice-paced encoding)
- Packet-level error concealment (selective intra refresh)
- Unified timing via a shared clock thread

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         ClockThread                                  │
│   (single source of truth for frame timing, slice pacing, etc.)     │
└───────────┬─────────────────────────┬───────────────────┬───────────┘
            │                         │                   │
            ▼                         ▼                   ▼
┌───────────────────┐   ┌─────────────────────┐   ┌────────────┐
│  FrameCapture     │   │  SlicePacedEncoder   │   │   Pacer    │
│  (reads YUV at    │   │  (encodes slice-by-  │   │            │
│   frame tick)     │   │   slice on schedule) │   │            │
└────────┬──────────┘   └──────────┬───────────┘   └─────┬──────┘
         │                         │                      │
         └────────►encode──────────┘                      │
                                   │                      │
                                   ▼                      ▼
                          ┌─────────────────────────────────────┐
                          │  NetworkSender                        │
                          │  (wraps UDP socket + NetworkSimulator)│
                          └──────────────────┬──────────────────┘
                                             │ UDP
                                             ▼
                          ┌─────────────────────────────────────┐
                          │  Receiver                             │
                          │  (DataReceiver + Decoder + Feedback)  │
                          └──────────────────┬──────────────────┘
                                             │ feedback (ACK/TWCC)
                                             ▼
                          ┌─────────────────────────────────────┐
                          │  CongestionController (GCC)          │
                          │  → outputs target_bitrate            │
                          │  → feeds Pacer + SlicePacedEncoder   │
                          └─────────────────────────────────────┘
```

## Components

### 1. ClockThread (New)

A dedicated timing thread that:
- Maintains a monotonic system clock reference
- Fires frame-tick callbacks at exact frame intervals (e.g., every 33.3ms for 30fps)
- Provides `GetCurrentTimeUs()` for any thread to query
- Provides slice sub-deadlines within each frame interval
- All other threads (capture, encoder, pacer) reference this clock for alignment

Design: single thread with a condition variable or timerfd-style loop. Other threads call `WaitForNextFrameTick()` or `GetSliceDeadline(slice_index, total_slices)`.

### 2. NetworkSender (Refactor of DataSender)

A class that wraps UDP socket send with an optional `NetworkSimulator`:
- Interface identical to current `DataSender::SendPacket` from the outside
- Internally: if simulator is enabled, packets pass through delay/loss/bandwidth shaping before actual `sendto()`
- Configuration: bandwidth cap (kbps), one-way delay (ms), packet loss rate (%), jitter (ms)
- Toggle: `EnableSimulator(config)` / `DisableSimulator()`
- Outside code (VideoCaptureAndSend, etc.) does not change

### 3. CongestionController (Interface + GCC Implementation)

Base class:
```cpp
class CongestionController {
 public:
  virtual void OnFeedback(const TransportFeedback& feedback) = 0;
  virtual int GetTargetBitrateKbps() const = 0;
  virtual void SetMinMaxBitrate(int min_kbps, int max_kbps) = 0;
};
```

GCC implementation (per the paper "A Google Congestion Control Algorithm for Real-Time Communication"):
- **Delay-based controller**: estimate one-way delay gradient using arrival-time filter, detect overuse/underuse via adaptive threshold, output "decrease/hold/increase" signal
- **Loss-based controller**: if loss > threshold, reduce bitrate proportionally
- **AIMD rate control**: increase multiplicatively when underuse, decrease on overuse signal
- **Bandwidth probing**: periodically enter a probe phase — temporarily send at a rate above the current estimate (e.g., 1.5×) to test if more capacity is available. If no overuse is detected during the probe window, raise the estimate to the probed rate. If overuse is detected, abort and revert. This ensures the controller can discover increased available bandwidth after a congestion dip, not just slowly ramp up.
- Feedback format: TWCC-style (per-packet receive timestamps sent back by receiver)

### 4. SlicePacedEncoder (Core Research Contribution)

Replaces the current `EncodeFrame()` single-shot call:
- Configure x264 with N slices (e.g., 100 row-based slices — x264 slices are horizontal MB ranges, not 2D tiles)
- Encode one slice at a time — each slice is independently encodable and sendable
- Between slices, idle until the next slice deadline (provided by ClockThread)
- After each slice is encoded, immediately send it (don't buffer the whole frame)
- Mid-frame bitrate adjustment: if CongestionController updates target during encoding, adjust QP for remaining slices

**x264 modification strategy (experiment first):**
1. First, experiment with x264's public API: `i_slice_count`, `b_sliced_threads`, slice callbacks — determine if per-slice NAL output is achievable without patching
2. If public API is insufficient (likely), modify `third_party/x264` internals to expose a per-slice encoding API:
   - Add `x264_encoder_encode_slice(x264_t*, x264_nal_t**, int* pi_nal, x264_picture_t*, int slice_idx)` — encodes a single slice and outputs its NAL
   - Or add a callback/flush mechanism that yields after each slice
3. Rebuild patched x264 (`lib/libx264.a`) and verify linking

Key x264 internals to modify (if needed):
- `encoder/encoder.c` — main encode loop, slice iteration
- `encoder/slicetype.c` — slice decision logic
- `common/frame.h` — per-slice state tracking

### 5. Selective Intra Refresh (Packet-Level Error Concealment)

When receiver reports a lost packet (which maps to a specific slice):
- Instead of forcing a full I-frame, mark only that slice region for intra refresh in the next frame
- Use x264's `x264_picture_t.prop.quant_offsets` or slice-level intra refresh
- Reduces recovery cost from O(full_frame) to O(one_slice)

### 6. Enhanced Feedback Protocol

Extend the current ACK-based feedback:
- Add TWCC-style feedback: receiver reports per-packet receive timestamps
- Add loss report: which (frame_seq, packet_index) were not received
- Feed into CongestionController and Selective Intra Refresh

## Implementation Order

1. ClockThread — foundation, everything else depends on timing
2. NetworkSender (with simulator) — needed to test congestion scenarios
3. Enhanced Feedback Protocol — GCC needs richer feedback, so feedback comes first
4. CongestionController interface + GCC — consumes feedback to produce bitrate estimates
5. Bandwidth Probing — builds on top of GCC for full utilization
6. SlicePacedEncoder — needs ClockThread for pacing deadlines; experiment with public API first, patch x264 if needed
7. Selective Intra Refresh — needs loss feedback + slice-aware encoder
8. Integration — wire all components, end-to-end test

## Constraints

- MVP first, no corner cases (per project rules)
- x264 only for now
- macOS + Linux build; local NetworkSimulator avoids mahimahi (Linux-only)
- Keep commits small (~200 lines)
- Task order within each sub-task: interface/spec → tests → implementation → integration

## Per-Phase Completion Criteria

After each phase is finished:
1. **Compile** — `make` succeeds with no errors on macOS + Linux
2. **Test** — all unit tests for that phase pass
3. **Summary** — write `phaseN_summary.md` documenting what was changed, key decisions, and any known limitations
4. **Commit** — one or more small commits covering the phase

## x264 Modification Strategy

Approach: experiment first, patch only if needed.
1. Task 3.1 experiments with the public x264 API to determine what's achievable without modifications
2. If public API cannot yield per-slice NALs independently (likely), Task 3.2 patches `third_party/x264`
3. Patched x264 must be rebuilt (`make` in `third_party/x264/`) and the resulting `lib/libx264.a` must be updated so the main project links against the modified version
4. The goal (true per-slice encode) is non-negotiable — but we prove the need before patching
