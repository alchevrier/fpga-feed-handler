# ADR-015 — Market Toxicity Detection: Quote Cancellation Ratio in Hardware

## Status
Proposed — Deferred (requires X/D/U message type support as prerequisite)

## Date
2026-06-04

## Context

A primary use case for a hardware order book is feeding a matching engine that generates orders. The matching engine needs to know not just the current best bid/ask, but whether the current market state is **toxic** — i.e. whether acting on the current quote carries elevated adverse selection risk.

The canonical toxicity signal in market microstructure is the **quote cancellation ratio**: the proportion of quoted volume that is cancelled rather than traded through. A high cancellation ratio in a short window indicates that liquidity providers are pulling quotes faster than they are being filled — a signal of informed order flow or imminent price movement. A matching engine that receives this signal can reduce aggression, widen its own quotes, or pause order generation entirely.

Computing this signal in software introduces latency between market data arrival and the matching engine's awareness of toxicity. Computing it in hardware, in the same pipeline that processes the feed, means the toxicity signal is updated at the same clock cycle as the book — the matching engine never acts on stale toxicity state.

**Prerequisite:**
Toxicity detection requires X (Order Cancel), D (Order Delete), and E (Order Executed) message types in addition to the current A (Add Order). The current kernel only handles A messages. This ADR is deferred until X/D/U support is implemented.

## Decision

Add a `toxicity_monitor` task as a parallel branch in the DATAFLOW pipeline, receiving the same `LevelUpdate` stream as `update_snapshot` (via stream fork or duplicate stream).

**Signal definition:**
For each instrument, over a rolling window of W messages:
- `n_add`: count of Add Order messages admitted
- `n_cancel`: count of Cancel/Delete messages admitted
- Toxicity level determined by bit-shift threshold comparisons (see below) — no division, no multiply

The window resets after W admitted messages (W a power-of-2, configurable via `s_axilite`).

**Threshold levels (pre-configured via `s_axilite`):**
Thresholds are expressed as bit-shift values `k0` and `k1` on the **cancellation-to-add ratio**. `n_cancel > (n_add >> k)` tests whether the cancellation rate exceeds `1/2^k` — one right shift on `n_add` and one comparator, no division:
```
Level 0 (clean):    n_cancel ≤ (n_add >> k0)  → cancel rate ≤ 1/2^k0
Level 1 (elevated): n_cancel >  (n_add >> k0)
                AND n_cancel ≤ (n_add >> k1)  → cancel rate between 1/2^k0 and 1/2^k1
Level 2 (toxic):    n_cancel >  (n_add >> k1)  → cancel rate > 1/2^k1
```
Example: `k0=3, k1=2` → elevated when cancel rate exceeds 12.5%, toxic when it exceeds 25%. `k0` and `k1` are written via `s_axilite` and updated daily by the ML batch job. Implementation: two right shifts + two comparators, fully combinatorial, no DSP.

The FPGA writes the resulting level (0/1/2) to `ToxicitySnapshot` alongside `BookSnapshot` each window reset. What the matching engine does with each level — reduce aggression, widen quotes, halt order generation — is matching engine policy, not defined here.

**Hardware implementation:**
- Two counters per instrument per window: `n_add` and `n_cancel` (both `ap_uint<16>`)
- On window reset: latch `n_add` and `n_cancel`, reset counters, begin new window
- Level computation: two right shifts on `n_add` + two comparators against `n_cancel`, fully combinatorial, 0-cycle latency, no DSP required
  ```
  level = (n_cancel > (n_add >> k1)) ? 2
        : (n_cancel > (n_add >> k0)) ? 1
        : 0;
  ```
- `k0`, `k1` are `ap_uint<4>` registers written via `s_axilite`
- Threshold comparison: two comparators, combinatorial, 0-cycle latency

**Output:**
`ToxicitySnapshot` register (alongside `BookSnapshot`):
```cpp
struct ToxicitySnapshot {
    ap_uint<16> n_cancel;      // raw cancel count for current window
    ap_uint<16> n_add;         // raw add count for current window
    ap_uint<2>  level;         // 0=clean, 1=elevated, 2=toxic
};
```
The host or ML batch job computes the exact ratio from `n_cancel` / `n_add` post-hoc — no fixed-point approximation stored on chip.

The toxicity level (0/1/2) is written to a dedicated output register alongside `BookSnapshot`. The matching engine reads both registers on the same clock cycle.

## Consequences

**Latency:**
`toxicity_monitor` runs as a parallel DATAFLOW branch — it does not add to the hot-path critical path. The toxicity signal is updated at the same cycle as `BookSnapshot`.

**Prerequisite dependency:**
Cannot be implemented until X/D/U message types are handled. The `n_cancel` counter has nothing to increment until Cancel and Delete messages flow through the pipeline.

**Window granularity:**
Fixed-point ratio over a configurable window is a coarse signal. For high-frequency instruments (AAPL, SPY) a short window may contain thousands of messages and the signal is statistically robust. For illiquid instruments a longer window is needed to accumulate enough samples before the ratio is meaningful. Per-instrument window sizing (via `s_axilite`) mitigates this — but choosing the right window and thresholds per symbol requires historical data.

**ML-driven threshold calibration:**
Toxicity thresholds (`threshold_0`, `threshold_1`) and window sizes (W) are not fixed constants — they depend on each symbol's microstructure, liquidity regime, and observed cancellation behaviour. An overnight ML batch job can analyse the previous session's captured `ToxicitySnapshot` history against realised fill outcomes (did elevated toxicity correctly predict adverse fills? were clean signals followed by profitable executions?) and produce updated per-symbol thresholds and window sizes for the next session. These are written to the `s_axilite` register map at session open. Same bitstream, learned configuration — the hardware signal is invariant; the calibration adapts daily.

**False positives at session open:**
At the start of a session, `n_add` is near zero. Small `n_cancel` values produce high ratios spuriously. A minimum message threshold (`n_add > min_messages` before the ratio is considered valid) prevents false toxic signals at session open. Default: `min_messages = 10`, configurable via `s_axilite`.

**Resource cost:**
Two counters per instrument slot = ~32 FFs. Two shift-and-compare operations = 2 LUT chains, no DSP. Total resource cost is negligible.

## Alternatives Considered

**Software toxicity detection:**
Compute the ratio in the host process reading the `BookSnapshot` over DMA. Adds one DMA round-trip latency (~microseconds) between the book update and the matching engine's awareness of the signal. For a hardware matching engine operating at nanosecond timescales, this lag is unacceptable. Rejected.

**Order flow imbalance (OFI) as toxicity proxy:**
OFI = (bid volume added − bid volume cancelled) − (ask volume added − ask volume cancelled). More sophisticated than cancellation ratio; captures directional pressure. Requires the same X/D/U prerequisite. Could replace or supplement the cancellation ratio signal. Deferred — implement cancellation ratio first as the simpler baseline, evaluate OFI as an extension.

**Fixed window vs exponential moving average:**
EMA weights recent messages more heavily than the fixed window. Hardware EMA requires a multiply per message on the hot path. Fixed window requires only counter increments. Fixed window chosen for simplicity; EMA deferred as an extension.
