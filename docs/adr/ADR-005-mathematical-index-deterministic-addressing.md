# ADR-005 — Deterministic BRAM Addressing: Mathematical Index over Binary Search

## Status
Accepted

## Date
2026-05-28

## Context

Given a price level value from an incoming ITCH message, the kernel must locate the corresponding entry in the BRAM order book. There are two broad approaches:

### Option A — Binary search
```cpp
// Find the BRAM index for a given price
int lo = 0, hi = num_levels - 1;
while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (book[mid].price == price) return mid;
    else if (book[mid].price < price) lo = mid + 1;
    else hi = mid - 1;
}
```

Binary search over N levels requires **O(log₂ N) iterations**. Each iteration contains a conditional branch. On an FPGA:

- There is no branch predictor. Each conditional branch synthesises to a multiplexer selecting the next loop variable value.
- The loop carries a dependency: iteration N+1 cannot begin until `lo`/`hi` are resolved in iteration N.
- This **loop-carried dependency breaks pipelining**. HLS cannot achieve II=1 on a loop whose body depends on the previous iteration's output.
- For 1024 price levels, binary search takes up to 10 iterations = 10+ cycles of loop-carried latency.
- The loop iteration count is **data-dependent** (terminates early on a hit). This means pipeline latency is non-deterministic — it varies with the input price value.
- **LUT/FF budget**: to pipeline the loop at all, HLS must unroll it — instantiating one copy of the comparator-and-mux logic per iteration. For 10 iterations, that is 10× the comparator and mux resources. On a budget-constrained device this exhausts LUTs before the rest of the design is placed. Even without unrolling, the loop variable registers and mux select chains consume fabric resources that scale with search depth.

### Option B — Mathematical index
NASDAQ ITCH instruments trade on a fixed tick grid: prices are integer multiples of a minimum tick size (`tick_size`), anchored at a known minimum price (`base_price`). Both values are transmitted in the ITCH Stock Directory (`R`) message before any Add Order messages for that instrument.

Given these constants, the BRAM index for any valid price is:

```cpp
// Host computes once at startup, writes via s_axilite:
//   inv_tick = (1u << 16) / tick_size
//
// Kernel hot path — no division:
uint16_t idx  = (uint16_t)(((price - base_price) * inv_tick) >> 16);
uint16_t addr = (side_bit << BOOK_ADDR_WIDTH) | idx;
```

This is **pure combinatorial logic**. There is no loop. There is no branch. There is no dependency on a previous computation.

The division is eliminated entirely: because `tick_size` is a constant known before the feed starts, the host precomputes its reciprocal as a fixed-point integer (`inv_tick = (1u << 16) / tick_size`) and writes it to the kernel once via `s_axilite`. The kernel replaces the divide with a **multiply + right-shift**.

On UltraScale+, a multiply is serviced by a **DSP48E2 slice** — a dedicated 27×18 signed multiply-accumulate block in the fabric. DSP48E2 slices run at 600+ MHz and consume zero LUTs. The right shift by 16 is pure wiring: no logic, no LUTs, no delay. The subtractor costs ~16 LUTs for a 16-bit price field.

The resource cost is fixed and minimal: one subtractor + one DSP48E2 + wiring for the side bit. The resource cost does not scale with book depth, tick size value, or price range.

The result: **II=1, fixed 1-cycle latency, independent of book depth and independent of the input price value**.

## Decision

Use the mathematical index formula `((price - base_price) * inv_tick) >> 16` to compute the BRAM address for a given price level. Binary search is not used.

The host precomputes `inv_tick = (1u << 16) / tick_size` from the ITCH Stock Directory (`R` message) and writes it to the kernel once via `s_axilite` before the feed starts. The kernel hot path never divides.

The side is encoded as a single address bit:
```cpp
// side_bit = 1 for bid, 0 for ask (or vice versa — fixed at synthesis)
uint16_t addr = (side_bit << BOOK_ADDR_WIDTH) | idx;
```

No branch, no mux over side: the address is a direct bit composition.

## Why This Is the Core Design Insight

This decision is what makes the 6-cycle latency target achievable. Without it:

| Approach | Stage latency | Deterministic? | Resource cost |
|---|---|---|---|
| Binary search (1024 levels) | ≥10 cycles | No — data-dependent | O(log₂ N) × comparator + mux (LUTs) per unrolled iteration |
| Mathematical index (divide) | 1 cycle | Yes | Fixed: subtractor + LUT divider (shift-add tree) |
| Mathematical index (multiply-shift) | 1 cycle | Yes | Fixed: subtractor (~16 LUTs) + 1 DSP48E2 + wiring |

The deeper principle is an FPGA design philosophy: **manipulate the signal, don't compute over it**. Binary search exists because on a CPU you don't know where the data is — you search at runtime. On an FPGA, the tick grid is a fixed property of the instrument known at initialisation. That knowledge can be encoded directly into the address wires: the index is not computed at runtime, it is *inherent in the signal* the moment you subtract `base_price` and normalise by `tick_size`. There is nothing to search. Using 10 loop iterations and a LUT tree to rediscover something that is already latent in the arithmetic of the price field is wasted fabric.

## Pre-conditions and Trade-offs

- **`base_price` and `inv_tick` must be known before the hot path starts.** The host computes both and writes them via `s_axilite` before starting the kernel:
  - `tick_size` is derived from exchange rules, not transmitted by NASDAQ. For NMS stocks ≥ $1.00, the minimum tick is $0.01 = **100** in ITCH fixed-point ($0.0001 units). The ITCH Stock Directory (`R`) message does not carry tick size or base price.
  - `base_price` is chosen by the operator — typically the prior close or the lowest price expected for the instrument during the session, rounded down to the nearest tick.
  - `inv_tick = (1u << 16) / tick_size` is then precomputed in software and written alongside `base_price`.
  - NASDAQ guarantees `R` messages precede any Add Orders for that instrument, so the host has confirmed the instrument's identity before the feed reaches the kernel's `ap_fifo` input.
- **`inv_tick` is an integer approximation of the reciprocal.** For tick sizes that are not powers of two, `(1u << 16) / tick_size` truncates. The host must verify that for the maximum expected price offset, `((price - base_price) * inv_tick) >> 16` maps to the correct integer index. For NASDAQ equity tick sizes (multiples of $0.0001 in fixed-point), this holds over the valid price range.
- **`MAX_LEVELS` must be sized at synthesis time.** The address width is fixed in the bitstream. An instrument whose traded price range exceeds `MAX_LEVELS` tick steps cannot be supported without recompilation. For the single-instrument scope of this project (ADR-008), this is known and bounded.

## Consequences

- `handle_event` receives a pre-computed address from `parse_message` via the `MarketEvent` struct — the index is computed in the parse stage where it costs 1 cycle alongside field extraction.
- No dynamic search structures (sorted arrays, trees) are present in the design.
- The design is not generalisable to instruments with variable tick sizes without a parameter reload mechanism (future ADR).

## Open Questions

**Out-of-bounds index behaviour.**
If `base_price` or `inv_tick` is misconfigured by the host, or if an ITCH message contains a price outside the expected range, the computed index may exceed `MAX_LEVELS`. On an FPGA, an out-of-range BRAM address wraps or aliases to another address — it does not trap or raise an exception. The kernel has no mechanism to detect or reject a bad index. Whether to add a bounds check (at the cost of an extra cycle and a conditional branch in the parse stage) or to treat this as a host pre-condition violation that is out of scope is not yet decided. For the current single-instrument, controlled-input scope (ADR-008), this is acceptable. A production design would require a guard.
