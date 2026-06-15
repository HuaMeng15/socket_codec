# Phase 2 Summary — Congestion Control

## What was done

### Task 2.1: Enhanced Feedback Protocol
- `TransportFeedback` struct: batch of per-packet receive timestamps (TWCC-style)
- `LossReport` struct: explicit lost packet list with frame+slice granularity
- `FeedbackCollector` (receiver): collects arrival times, sends batched feedback
  at configurable interval (default 20 packets), loss detection via received_mask
- `FeedbackHandler` (sender): parses new message types via `FeedbackMessageType`
  header byte, dispatches via callbacks
- Wire format: `FeedbackMessageHeader + PacketArrivalRecord/PacketLossRecord`
- Backward compatible: legacy ACKs still parsed and forwarded as single-entry feedback

### Task 2.2: CongestionController + GCC
- `CongestionController` base interface: `OnTransportFeedback`, `OnLossReport`, `GetTargetBitrateKbps`
- `GccController` implementation:
  - Delay-based: inter-group arrival gradient (EMA, α=0.9), adaptive threshold overuse
    detector, AIMD rate control (×0.85 decrease, +100kbps increase)
  - Loss-based: windowed loss ratio, multiplicative decrease (×0.8) above 10% loss
  - Final rate = min(delay-based, loss-based), clamped to [min, max]
- Wired into sender: `FeedbackHandler` → callbacks → `GccController` → `Encoder.SetTargetBitrate` + `Pacer.SetTargetBitrate`

### Task 2.3: Bandwidth Probing
- `BandwidthProber`: state machine (Idle→Probing→Evaluating→Committed|Aborted)
- Triggers after 3s stable (configurable), probes at 1.5× current rate
- Aborts on 2+ overuse signals during probe; commits if evaluation clean
- Integrated into GCC: overuse/stable feed prober, prober effective rate used in final bitrate

## Test Results
- `make unit_test`: 30/30 passing
- `make`: compiles successfully
- `scripts/run_local.sh mock 30 30 5000 --sim_bandwidth_kbps=5000`: pipeline works with bandwidth cap

## Unit Tests Added
- `transport_feedback_test.cc`: 5 tests (serialize/deserialize, loss detection, type dispatch)
- `gcc_controller_test.cc`: 6 tests (initial rate, stable, overuse decrease, loss decrease, bounds, recovery)
- `bandwidth_prober_test.cc`: 6 tests (idle, no probe after overuse, start probe, commit, abort, multiplier)

## Commits
- `6bdb5bf` Add TWCC-style transport feedback and loss reporting
- `be746a0` Implement GCC congestion controller with delay+loss-based rate control
- `d63dee6` Add bandwidth probing to congestion controller

## Notes
- Integration tests with bandwidth step-change plots (10→1Mbps etc.) are ready to run
  using `--sim_bandwidth_kbps` flag. The plotting infrastructure from Phase 0
  (`scripts/draw.py`) can visualize bitrate adaptation once the receiver integrates
  `FeedbackCollector` to send TWCC feedback (currently still using legacy ACKs in the
  actual receiver handler — full wiring is an integration task for Phase 5).
- GCC uses real wall-clock time for rate-limit pacing — the test suite sleeps to
  simulate passage of time. In production, feedback arrives at network rate.
