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

### Task 2.2: CongestionController + GCC (WebRTC-aligned)
- `CongestionController` base interface: `OnTransportFeedback`, `OnLossReport`, `GetTargetBitrateKbps`
- `GccController` implementation aligned with WebRTC source:

  **Delay-based (trendline_estimator.cc / delay_based_bwe.cc):**
  - Trendline estimator: linear least-squares regression over sliding window
    (`kTrendlineWindowSize = 20` samples)
  - Accumulated delay per batch, smoothed with `kTrendlineSmoothingCoeff = 0.9`
  - Overuse detection: `slope × kThresholdGain(4.0)` vs adaptive threshold
  - Adaptive threshold: grows at `k_up = 0.0087/ms` during overuse,
    decays at `k_down = 0.039/ms` toward `kMinThreshold = 6ms`, range [6, 600] ms
  - Startup scaling: reduced sensitivity for first 10 delta samples
  - Rate control AIMD: multiplicative decrease `×0.85` on overuse,
    additive increase `~8%/second` during normal/underuse (with 1s post-overuse cooldown)

  **Loss-based (send_side_bandwidth_estimation.cc):**
  - `loss < 2%`: additive increase allowed
  - `2% ≤ loss < 10%`: hold (no change)
  - `loss ≥ 10%`: decrease by `(1 - 0.5 × loss_fraction)`
    (e.g., 10% loss → 5% decrease, 20% loss → 10% decrease)

  **Combined:** `final_rate = min(delay_based, loss_based)`, clamped to [min, max].
  Prober temporarily overrides when actively probing.

- Wired into sender: `FeedbackHandler` → callbacks → `GccController` → `Encoder.SetTargetBitrate` + `Pacer.SetTargetBitrate`

### Task 2.3: Bandwidth Probing (WebRTC-aligned)
- `BandwidthProber`: WebRTC-style probe controller with three trigger modes:

  **1. Initial exponential probing (startup):**
  - First probe at `3×` initial estimate, second at `6×`
  - If probe succeeds (estimated > `kFurtherProbeThreshold(0.7)` × target),
    further probe at `2×` successful estimate
  - Disabled after 2 exponential probes or on overuse

  **2. ALR (Application Limited Region) periodic probing:**
  - Every `kAlrProbeIntervalMs = 5000ms` when app is below estimated capacity
  - Probe target: `1.5×` current estimate

  **3. Bitrate drop recovery probing:**
  - Triggered when bitrate drops by ≥66% (`kBitrateDropThreshold`)
  - Probe at `kProbeFractionAfterDrop = 0.85` of pre-drop bitrate
  - Timeout after `kBitrateDropTimeoutMs = 5000ms`

  **Probe targets adapt to network:**
  - Capped by `max_bitrate`
  - Scaled from current estimate (not fixed absolute)
  - Overuse cancels active probe + 1s cooldown

- Integrated into GCC: overuse signals feed prober, prober overrides rate only when active

## Test Results
- `make unit_test`: 33/33 passing
- `make`: compiles successfully
- `scripts/run_local.sh mock 30 30 5000 --sim_bandwidth_kbps=5000`: pipeline works with bandwidth cap

## Unit Tests Added
- `transport_feedback_test.cc`: 5 tests (serialize/deserialize, loss detection, type dispatch)
- `gcc_controller_test.cc`: 7 tests (initial rate, stable, overuse detection + decrease,
  loss below 2%, loss above 10%, bounds enforcement, overuse behavior)
- `bandwidth_prober_test.cc`: 7 tests (idle, no probe after overuse, exponential 3x,
  successful probe triggers further, overuse cancels, max cap, ALR, drop recovery)

## Commits
- `6bdb5bf` Add TWCC-style transport feedback and loss reporting
- `be746a0` Implement GCC congestion controller with delay+loss-based rate control
- `d63dee6` Add bandwidth probing to congestion controller
- `ec9dfe7` Rewrite BandwidthProber to match WebRTC probe_controller design
- `36a3598` Align GCC delay and loss-based CC with WebRTC implementation

## References
- [trendline_estimator.cc](https://os.unplugged.com/werunplugged/unplugged-system/src/branch/main/external/webrtc/modules/congestion_controller/goog_cc/trendline_estimator.cc) — smoothing_coeff=0.9, threshold_gain=4.0, window_size=20
- [send_side_bandwidth_estimation.cc](https://os.unplugged.com/werunplugged/unplugged-system/raw/branch/main/external/webrtc/modules/congestion_controller/goog_cc/send_side_bandwidth_estimation.cc) — loss thresholds 2%/10%, decrease formula
- [probe_controller.cc](https://os.unplugged.com/werunplugged/unplugged-system/src/branch/main/external/webrtc/modules/congestion_controller/goog_cc/probe_controller.cc) — exponential probing, ALR, drop recovery
- [Loss based bandwidth estimation in WebRTC](https://medium.com/@ggarciabernardo/loss-based-bandwidth-estimation-in-webrtc-8d650f72bb42) — original loss-based logic explanation

## Notes
- Integration tests with bandwidth step-change plots (10→1Mbps etc.) are ready to run
  using `--sim_bandwidth_kbps` flag. The plotting infrastructure from Phase 0
  (`scripts/draw.py`) can visualize bitrate adaptation once the receiver integrates
  `FeedbackCollector` to send TWCC feedback (currently still using legacy ACKs in the
  actual receiver handler — full wiring is an integration task for Phase 5).
- GCC uses real wall-clock time for rate-limit pacing — the test suite uses sleeps to
  simulate passage of time. In production, feedback arrives at network cadence.
- Recovery from overuse requires the trendline window (20 samples) to flush with
  stable delay before the rate controller increases. This matches WebRTC's
  conservative recovery behavior.
