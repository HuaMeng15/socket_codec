# GCC Test Cases — Plain-English Guide

This document summarizes what the GCC (Google Congestion Control) tests check
and why. Tests live in `tests/gcc_controller_test.cc` and
`tests/bandwidth_prober_test.cc`. All unit tests use a **fake clock** so they
run fast and deterministically (no real waiting).

---

## 1. GCC Controller (`gcc_controller_test.cc`)

The controller blends three signals into one target bitrate: a **delay-based**
estimate (trendline + overuse detector + AIMD), a **loss-based** estimate, and
**bandwidth probing**. Tests are grouped by which signal they exercise.

### Test setup
- Clock starts at 10s (avoids startup edge cases).
- Initial bitrate 5000 kbps, range [100, 30000] kbps.
- Acked throughput pinned high (30000) by default so the AIMD increase cap
  (1.5× acked) doesn't interfere — overuse tests lower it to model a saturated
  link.
- Helpers feed synthetic feedback: `FeedStable` (send/arrival spacing equal —
  no queue change), `FeedOveruse` (arrivals spread wider than sends — queue
  growing), `FeedUnderuse` (arrivals tighter than sends — queue draining).

### Basic state
| Test | What it verifies |
|------|------------------|
| `InitialBitrate` | Target starts at the configured 5000 kbps. |
| `BitrateStaysWithinBounds` | Even under 100% loss, target never drops below the floor or above the ceiling. |

### Delay-based — overuse (queue building up)
| Test | What it verifies |
|------|------------------|
| `OveruseDetectedOnGrowingDelay` | Steadily growing one-way delay makes the target drop below where it started. |
| `OveruseDecreasesMultiplicatively` | The drop is multiplicative (≥ one 0.85× cut), not linear, and doesn't crash to the floor. |
| `StartupWarmupPreventsEarlyOveruse` | With very few samples, sensitivity is scaled down so a tiny noisy batch can't trigger a false overuse. |

### Delay-based — underuse (queue draining)
| Test | What it verifies |
|------|------------------|
| `UnderuseDetectedOnDecreasingDelay` | After an overuse drop, a draining queue + recovered throughput lets the rate climb back up. |

### Delay-based — stable network
| Test | What it verifies |
|------|------------------|
| `StableNetworkIncreasesRate` | ~1s of clean, no-delay-change feedback nudges the rate upward (additive increase). |

### Delay-based — adaptive threshold
| Test | What it verifies |
|------|------------------|
| `ThresholdAdaptsUpDuringOveruse` | The overuse threshold decays toward 6ms when stable, then grows back (capped at 600ms) during sustained overuse — matching WebRTC's adaptive `k_up`/`k_down`. |

### Delay-based — noise rejection
| Test | What it verifies |
|------|------------------|
| `NoisyFeedbackDoesNotFalsePositive` | Mean-zero jitter (±1ms) stays within threshold, so the rate holds and no overuse fires. |

### Loss-based — three reaction ranges (WebRTC rules)
| Test | Loss | Expected behavior |
|------|------|-------------------|
| `LossBelow2PercentAllowsIncrease` | 1% | < 2% → increase allowed (loss-based tracks delay-based). |
| `LossBetween2And10PercentHolds` | 5% | 2–10% → hold, no change. |
| `LossAbove10PercentDecreases` | 20% | ≥ 10% → cut by `(1 − 0.5·loss)` → ~4500 kbps. |
| `HighLossDecreasesProportionally` | 50% | Same rule → ~3750 kbps (0.75×). |

### Transitions & floor
| Test | What it verifies |
|------|------------------|
| `OveruseToNormalTransition` | After overuse, returning to stable feedback flushes the window, overuse counter resets to 0, and the rate recovers instead of dropping further. |
| `MinBitrateIsRespected` | 100 rounds of heavy overuse can't push the target below the configured 500 kbps floor. |

### Probe resolution (WebRTC: commit from received rate, no fixed window)
| Test | What it verifies |
|------|------------------|
| `ProbeCommitsFromReceivedRateWithoutWaiting` | A probe commits within a couple of feedback batches (event-driven on the first received-rate sample), not after a fixed time window. |
| `ProbeCommitsCapacityWhenLinkSaturated` | When the received rate is below the probe target (link saturated), the probe commits ~0.95× the measured capacity — never the over-target send rate. |

---

## 2. Bandwidth Prober (`bandwidth_prober_test.cc`)

The prober actively pushes extra traffic to discover spare capacity, mirroring
WebRTC's `probe_controller`. State machine: **Idle → Probing → WaitingForResult**.

### Test setup
- Estimated bitrate 5000 kbps, max 30000 kbps, clock at 10s.

| Test | What it verifies |
|------|------------------|
| `StartsIdle` | No probing until something triggers it. |
| `InitialExponentialProbeAt3x` | First probe targets 3× the estimate (15000). |
| `SecondExponentialProbeAt6x` | If the first probe's result is below the 0.7 "keep going" threshold, the next exponential probe jumps to 6× (30000, capped at max). |
| `SuccessfulProbeTriggersNextProbe` | A strong result (≥ 0.7 of target) triggers a follow-up probe at 2× the measured rate. |
| `DoesNotProbeAfterRecentOveruse` | Probing is suppressed right after an overuse event. |
| `OveruseCancelsProbe` | An overuse signal mid-probe aborts it back to Idle. |
| `ProbeTargetCappedByMax` | A 3× target above the ceiling is clamped to the max bitrate. |
| `NoProbeWhenNearMax` | If already at ~96% of max, don't bother probing. |
| `AlrProbingAfterInterval` | In application-limited regions, probe at 1.5× after the ALR interval (5s) elapses. |
| `DropRecoveryProbe` | After a sharp estimate drop (below 0.66× prior) **and** an underuse signal (queue draining), probe at 0.85× of the pre-drop rate to recover quickly. |
| `NoDropRecoveryProbeWhileStillCongested` | A sharp drop with **no** underuse signal does not probe — re-probing into a still-congested link would worsen it. |
| `ProbeTimesOut` | A probe with no result within 3s reverts to Idle. |
| `FailedProbeStopsInitialProbing` | A failed result ends the initial exponential probing sequence. |
| `GetPendingProbesTransitionsToWaiting` | Reading pending probes moves Probing → WaitingForResult; a second read returns empty. |

---

## 3. Integration Scenarios (`scripts/run_gcc_integration.sh`)

Beyond unit tests, three end-to-end scenarios run the full sender/receiver over
the async network simulator and plot the result (`docs/gcc_results/`):

| Scenario | Link | What it shows |
|----------|------|---------------|
| `static_10mbps` | constant 10 Mbps | Probe ramp from 500 kbps startup toward link capacity. |
| `drop_10to1` | 10 Mbps → 1 Mbps | Fast reaction to a capacity drop. |
| `static_1mbps` | constant 1 Mbps | Tight tracking with low queuing delay (~5ms). |

Each plot overlays target / delay-based / loss-based bitrate, link capacity, and
real per-frame latency (`recv decode time − send time` from the logs).

---

## Running the tests

```bash
make unit_test       # build + run all gtest unit tests
./scripts/run_gcc_integration.sh   # run the 3 integration scenarios + plots
```
