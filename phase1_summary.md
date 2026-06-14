# Phase 1 Summary — Foundations

## What was done

### Task 1.1: ClockThread
- `tools/clock_thread.h/.cc`: single timing source for the whole pipeline
- `GetCurrentTimeUs()`: monotonic microseconds since Start()
- `WaitForNextFrameTick()`: blocks until next frame boundary, returns frame index
- `GetSliceDeadline(frame, slice)`: evenly distributes slice deadlines across frame interval
- Integrated into `VideoCaptureAndSend::Run()` — replaces the old `sleep_for` hack
- 7 unit tests covering timing accuracy, sequential ticks, stop behavior, concurrency

### Task 1.2: NetworkSender + NetworkSimulator
- `transmission/network_simulator.h/.cc`: configurable network impairment
  - Token-bucket bandwidth limiting (blocks sender to simulate link capacity)
  - Propagation delay (sleeps before send)
  - Random uniform packet loss
  - Jitter on delay
  - Dynamic bandwidth changes at runtime
- `transmission/network_sender.h/.cc`: thin wrapper around `send()` + optional simulator
- Plugged into `DataSender::SendPacket` — transparent when no simulator attached
- 6 unit tests covering all simulator behaviors

## Test Results
- `make unit_test`: 13/13 passing
- `make`: compiles successfully
- `scripts/run_local.sh mock 30 30`: full pipeline sender→receiver works

## Build Changes
- Added `unit_test` target to Makefile (gtest from `/opt/homebrew`)
- Created `tests/` directory for unit tests
- Test pattern rule placed before generic rule for correct gtest include path

## Commits
- `26bf741` Add ClockThread with frame-tick and slice-deadline support
- `4983216` Add NetworkSender with pluggable NetworkSimulator
