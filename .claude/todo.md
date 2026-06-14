# Socket Codec — TODO

## Phase 0: Build System Refactor

### Task 0.1: Restructure x264 build integration
- [ ] Remove pre-built `libx264.a` from link path (`./lib`)
- [ ] Add build step for `third_party/x264`: configure + make produces `third_party/x264/libx264.a`
- [ ] Update main Makefile: change `-L./lib` to include `-L./third_party/x264` for x264 linkage
- [ ] Update include path to use `third_party/x264/` headers instead of `include/x264/`
- [ ] Verify `make clean && make` compiles and links successfully on macOS
- [ ] Git commit: "Refactor build to compile x264 from third_party source"

### Task 0.2: One-tap build & run scripts
- [ ] Write `scripts/build.sh`: clean build of x264 + main project (single command)
- [ ] Write `scripts/run_local.sh`: run sender+receiver locally without mahimahi (for macOS dev)
- [ ] Verify both scripts work end-to-end
- [ ] Git commit: "Add one-tap build and local run scripts"

### Phase 0 Completion Gate
- [ ] `scripts/build.sh` succeeds on macOS
- [ ] `scripts/run_local.sh` runs sender+receiver locally
- [ ] Write `phase0_summary.md`

## Phase 1: Foundations

### Task 1.1: ClockThread
- [ ] Design `ClockThread` interface (header file with public API)
- [ ] Write unit tests for timing accuracy (frame ticks, slice deadlines)
- [ ] Implement monotonic clock with `GetCurrentTimeUs()`
- [ ] Implement frame-tick mechanism (`WaitForNextFrameTick()`)
- [ ] Implement slice-deadline calculation (`GetSliceDeadline(slice_idx, total_slices)`)
- [ ] Integrate ClockThread into existing `VideoCaptureAndSend::Run()` loop (replace `std::this_thread::sleep_for`)
- [ ] Git commit: "Add ClockThread with frame-tick and slice-deadline support"

### Task 1.2: NetworkSender (with NetworkSimulator)
- [ ] Design `NetworkSimulator` interface (bandwidth cap, delay, loss, jitter)
- [ ] Design `NetworkSender` class that wraps UDP socket + optional simulator
- [ ] Write unit tests (verify bandwidth limiting, delay injection, packet loss)
- [ ] Implement `NetworkSimulator`: token-bucket for bandwidth, queue + timer for delay, random drop for loss
- [ ] Implement `NetworkSender`: transparent pass-through when simulator disabled
- [ ] Replace `DataSender::SendPacket` internal usage with `NetworkSender`
- [ ] Verify existing pipeline still works unchanged when simulator is disabled
- [ ] Git commit: "Add NetworkSender with pluggable NetworkSimulator"

### Phase 1 Completion Gate
- [ ] `make` compiles successfully (macOS + Linux)
- [ ] All Phase 1 unit tests pass
- [ ] Write `phase1_summary.md`

## Phase 2: Congestion Control

### Task 2.1: Enhanced Feedback Protocol
- [ ] Define `TransportFeedback` struct (per-packet sequence + receive timestamp)
- [ ] Define `LossReport` struct (list of lost frame_seq + packet_index)
- [ ] Write tests: serialize/deserialize feedback, simulated loss detection
- [ ] Extend receiver to collect per-packet arrival times
- [ ] Extend receiver to detect and report losses (timeout-based)
- [ ] Implement TWCC-style feedback sender on receiver side
- [ ] Implement feedback parser on sender side (in `FeedbackHandler`)
- [ ] Git commit: "Add TWCC-style transport feedback and loss reporting"

### Task 2.2: CongestionController Interface + GCC
- [ ] Define `CongestionController` base class interface
- [ ] Write tests: feed synthetic delay patterns, verify bitrate output behavior
- [ ] Implement arrival-time delay gradient estimator (Kalman filter or linear regression)
- [ ] Implement overuse detector with adaptive threshold
- [ ] Implement AIMD rate controller (increase on underuse, decrease on overuse)
- [ ] Implement loss-based bitrate reducer
- [ ] Combine delay-based and loss-based into final `GetTargetBitrateKbps()`
- [ ] Wire `FeedbackHandler` → `CongestionController` → Pacer + Encoder
- [ ] Git commit: "Implement GCC congestion controller with delay+loss-based rate control"

### Task 2.3: Bandwidth Probing
- [ ] Design probe state machine (Idle → Probing → Evaluating → Committed/Aborted)
- [ ] Write tests: verify probe discovers available headroom when network has spare capacity
- [ ] Write tests: verify probe aborts and reverts when network is already saturated
- [ ] Implement probe trigger logic (when to start a probe: stable state for N seconds, no recent overuse)
- [ ] Implement probe execution: temporarily increase send rate to probe_rate (e.g., 1.5× current estimate)
- [ ] Implement probe evaluation: check feedback during probe window for overuse signals
- [ ] Implement probe result handling: commit higher rate on success, revert on overuse detection
- [ ] Integrate probing into `CongestionController` — probe result feeds into `GetTargetBitrateKbps()`
- [ ] Git commit: "Add bandwidth probing to congestion controller"

### Phase 2 Completion Gate
- [ ] `make` compiles successfully
- [ ] All Phase 2 unit tests pass
- [ ] Integration test: static 10Mbps — record target_bitrate over time, verify convergence to ~10Mbps; plot bitrate + delay curves
- [ ] Integration test: bandwidth drop 10Mbps → 1Mbps mid-stream — verify controller reacts (bitrate decreases, delay stabilizes); plot bitrate + delay curves
- [ ] Integration test: static 1Mbps — verify stable low-bitrate operation; plot bitrate + delay curves
- [ ] (Use all-zero YUV frames to produce predictable encoded sizes for controlled bandwidth measurement)
- [ ] Write `phase2_summary.md` (include plots from integration tests)

## Phase 3: Slice-Paced Encoding

### Task 3.1: x264 Slice Capability Experiment
- [ ] Study x264 `encoder/encoder.c` slice encoding loop (understand how slices are iterated)
- [ ] Write a standalone test: encode frame with `i_slice_count=100`, verify output has 100 NALs (slices are row-based, not 2D tiles)
- [ ] Test changing `i_slice_count` between frames (e.g., frame0=100, frame1=4)
- [ ] Test if x264 public API (`b_sliced_threads`, callbacks) can yield per-slice NALs without internal patches
- [ ] Document findings: what the public API can/cannot do
- [ ] Git commit: "Add x264 slice experiment tests and document findings"

### Task 3.2: x264 Per-Slice Encode API (internal modification)
- [ ] Design new x264 API: `x264_encoder_encode_slice()` — encodes single slice, outputs one NAL
- [ ] Write test: call `x264_encoder_encode_slice()` N times = same output as `x264_encoder_encode()` once
- [ ] Implement the per-slice encode API in `third_party/x264/encoder/encoder.c`
- [ ] Add necessary state tracking in x264 internals (per-slice context, partial frame state)
- [ ] Rebuild patched x264: update Makefile/build to rebuild `lib/libx264.a` from `third_party/x264`
- [ ] Verify patched x264 links correctly and existing encode still works
- [ ] Document modifications in `phase3_x264_notes.md`
- [ ] Git commit: "Patch x264 internals to expose per-slice encode API"

### Task 3.3: SlicePacedEncoder
- [ ] Design `SlicePacedEncoder` interface (extends or replaces `Encoder`)
- [ ] Write tests: verify slices are produced at correct time intervals
- [ ] Write tests: verify mid-frame bitrate change affects remaining slice sizes
- [ ] Implement slice-by-slice encode loop using new x264 per-slice API
- [ ] Integrate with ClockThread: wait for slice deadline before encoding next slice
- [ ] Send each slice immediately after encoding (call DataSender per slice)
- [ ] Implement mid-frame QP adjustment when target bitrate changes during encoding
- [ ] Git commit: "Add SlicePacedEncoder with per-slice timing and mid-frame QP adjustment"

### Phase 3 Completion Gate
- [ ] `make` compiles successfully (including rebuilt patched x264)
- [ ] All Phase 3 unit tests pass
- [ ] Write `phase3_summary.md`

## Phase 4: Error Concealment

### Task 4.1: Selective Intra Refresh
- [ ] Design loss-to-slice mapping and intra refresh interface
- [ ] Write test: simulate single slice loss, verify only that region is intra-coded in next frame
- [ ] On receiver: map lost packet back to (frame_seq, slice_index)
- [ ] Send loss report to sender with slice-level granularity
- [ ] On sender: mark lost slice region for intra refresh in next frame (x264 `intra_refresh` or `quant_offsets`)
- [ ] Compare: measure bitrate spike of selective refresh vs full I-frame
- [ ] Git commit: "Add selective intra refresh for packet-level error concealment"

### Phase 4 Completion Gate
- [ ] `make` compiles successfully
- [ ] All Phase 4 unit tests pass
- [ ] Write `phase4_summary.md`

## Phase 5: Integration

### Task 5.1: End-to-End Wiring
- [ ] Integrate all components into the main pipeline
- [ ] Verify: NetworkSimulator drops packets → loss report → selective intra refresh
- [ ] Verify: NetworkSimulator adds delay → GCC reduces bitrate → pacer + encoder adapt
- [ ] Verify: slice-paced encoding spreads packets evenly across frame interval
- [ ] Add command-line flags for simulator config (bandwidth, delay, loss)
- [ ] Run full pipeline test: 30s video with varying network conditions
- [ ] Git commit: "Integrate full RTC pipeline with all components wired"

### Phase 5 Completion Gate
- [ ] `make` compiles successfully
- [ ] Full end-to-end test passes
- [ ] Write `phase5_summary.md`

---

## Notes
- Task ordering within each sub-task: interface/spec → tests → implementation → integration
- Each task should result in ≤200 line commits
- x264 is the only codec for MVP
- Don't optimize corner cases; get basic function working first
- **After each phase**: compile, run tests, write `phaseN_summary.md`, then commit
- **x264 slices**: row-based (not 2D tile grid); "100 slices" means 100 horizontal slice rows
- **x264 modification**: experiment with public API first (Task 3.1); if insufficient, patch internals (Task 3.2)
- **Build**: patched x264 must be rebuilt and linked (`lib/libx264.a` from `third_party/x264`)
- **Platform**: macOS + Linux build; local NetworkSimulator avoids mahimahi (which is Linux-only)
