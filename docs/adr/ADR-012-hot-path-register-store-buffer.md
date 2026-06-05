# ADR-012 — Hot-Path Register Book: Eviction-Based Top-N Register File

## Status
Proposed — Deferred (partially superseded by implementation; see Context)

## Date
2026-06-04
Updated: 2026-06-05

## Context

The original `handle_event` stage maintained full order book depth in a static BRAM array. Every admitted message triggered a BRAM read-modify-write: read the existing level, add the new quantity, write back. This was 4 cycles, non-pipelined, and sat on the hot path. The RAW (read-after-write) hazard — the next message cannot begin its BRAM read until the previous write completes — was the sole cause of the kernel's II=16.

The matching engine does not need full depth in real time. It needs the top levels — best bid, best ask, and a shallow view of the near-touch. A risk engine or spread-capture strategy makes decisions on the top of book, not on a price level 200 ticks away.

**What was actually implemented (2026-06-05):**

`handle_event` was removed entirely and replaced by a DATAFLOW fork architecture:

- `parse_add_event` fans out to two parallel streams: `to_snapshot` and `to_book`.
- `update_snapshot` (hot path): reads `to_snapshot`, maintains a single running best bid and best ask via a combinatorial max/min compare against the current `snap` register. 0 cycles latency, II=1, 3.640 ns — purely combinatorial. No register file scan, no BRAM access.
- `register_book_update` (parallel path): reads `to_book` (depth=512 FIFO, absorbs burst back-pressure), computes the BRAM address via `idx = (price × inv_tick) >> 16 - base_offset`, writes price and accumulates qty. 10 cycles latency, II=1 via `#pragma HLS DEPENDENCE variable=levels inter false`.

The FIFO decoupling — not register eviction — is what removes BRAM from the hot path. `update_snapshot` never waits for `register_book_update`. Both run concurrently under `#pragma HLS DATAFLOW`. The kernel achieves II=1 (250M msg/s theoretical throughput), and the hot path latency is 2 cycles (8 ns at 250 MHz): arbitrate + parse_add_event + update_snapshot. CoSim verified PASS.

The eviction-based 16-register file described in this ADR remains a valid further enhancement — it would give the matching engine a ranked near-touch view (top-8 per side) rather than just best bid/ask. It is deferred until the current pipeline is extended to cancel/delete/replace message types, which are the prerequisite for register file promotion logic.

## Decision

Replace the single-stage BRAM read-modify-write with a **16-register file** and an eviction-based update policy.

**Structure:**
- `bids[8]`: 8 flip-flop registers, each holding `{price: ap_uint<32>, qty: ap_uint<32>}`. Best bid = max(bids[*].price).
- `asks[8]`: 8 flip-flop registers, each holding `{price: ap_uint<32>, qty: ap_uint<32>}`. Best ask = min(asks[*].price).

The register file is the live order book for all hot-path consumers. The matching engine reads from these registers directly — it never touches BRAM during the trading session.

**Update logic (per Add Order message, side=ASK, price P, qty Q):**

1. **Price match scan** — compare P against all 8 ask slot prices in parallel (8-wide comparator tree, 3 levels deep, fully combinatorial). If a match is found at index `i`, increment `asks[i].qty += Q`. Done — no BRAM access.

2. **Worst slot** — find the slot with the highest ask price (max of 8, same 7-comparator tree pattern). Call it `worst_price`, `worst_idx`.

3. **In-range** — if `P < worst_price`, the new level belongs in the top-8. Evict `asks[worst_idx]` to BRAM (single BRAM write, address computed via `idx = (worst_price - base_price) * inv_tick`), then insert `{P, Q}` at `worst_idx`.

4. **Out-of-range** — if `P >= worst_price`, the new level is outside the top-8. Write `{P, Q}` directly to BRAM. Register file unchanged.

Bids mirror asks: worst bid = min(bids[*].price). In-range insert evicts the lowest-priced bid slot.

**Critical path analysis:**

| Operation | Logic | Cycles |
|-----------|-------|--------|
| Price match scan | 8 comparators, 3-level tree | combinatorial |
| Worst slot find | 7 comparators, 3-level tree | combinatorial |
| Register write (update or insert) | 1 FF write | 1 |
| BRAM write (eviction or out-of-range) | 1 write port, no read | 1 |

BRAM write-only has no RAW hazard — consecutive evictions at different addresses are independent. The pipeline achieves II=1.

**BRAM role:**
BRAM holds price levels outside the top-8 per side — the cold depth record. It is written on eviction (when a level exits the register file) or on out-of-range arrival (when a level is too far from the touch to warrant a register slot). No BRAM read occurs on the hot path. BRAM is a persistence artifact: end-of-day reconstruction and crash recovery. Any downstream system needing full historical depth reads BRAM post-session.

```
Hot path:
  parse_add_event
       │ MarketEvent
       ▼
  register_book_update        ← 16 flip-flop registers (8 bid, 8 ask)
       │ (eviction on in-range miss)        │ (out-of-range)
       ▼                                    ▼
  BRAM write (evicted level)    BRAM write (far-touch level)
       │
  update_snapshot             ← reads best bid/ask from register file
```

## Consequences

**Hot path latency:**
BRAM read removed entirely from the hot path. All in-path logic is combinatorial (comparator trees) plus one register write. See ADR-013 for II impact.

**Best bid/ask accuracy:**
The register file always contains the top-8 levels per side. `update_snapshot` reads `min(asks[*].price)` and `max(bids[*].price)` — two more 7-comparator trees, trivially parallel. Best bid and best ask are always register-resident and always reflect the most recent admitted message.

**Depth accuracy:**
Levels outside the top-8 are in BRAM. BRAM is written immediately on eviction — there is no staleness window between the register file and BRAM. The full book is consistent at all times: registers hold the top-8, BRAM holds the rest.

**No flush trigger, no watchdog:**
The SPSC ring, message-count flush threshold, and cycle watchdog from the previous design are eliminated. BRAM is written exactly when a level leaves the register file — no batching, no timer, no configuration parameter. The cold path is reduced to a single BRAM write port.

**Resource impact:**
16 registers × 64 bits = 1,024 flip-flops. Four 7-comparator trees (match scan bid, match scan ask, worst-bid find, worst-ask find) ≈ 112 LUTs. Negligible on xa7a12t.

**Add-Order only (this phase):**
Qty updates for existing levels (same price, new qty from a modify/cancel message) require the price-match scan path. The eviction path handles inserts only. Cancel (`X`), Delete (`D`), and Replace (`U`) message types — which reduce or remove qty at a price level — are deferred to roadmap step 5. When a cancelled level's qty reaches zero it must be cleared from the register file and a cold-depth level promoted into the vacancy; that promotion logic is out of scope here.

## Alternatives Considered

**SPSC ring buffer with periodic flush (previous design):**
Defers BRAM writes to a cold-path thread triggered by message count or cycle watchdog. Adds flush threshold configuration, staleness window, and ML calibration of M and T. The eviction model is strictly simpler: BRAM writes are triggered by natural market events (levels leaving the top-8), not by timers or counts. Rejected in favour of eviction.

**Full BRAM with pipeline forwarding to resolve RAW hazard:**
Add a forwarding register that captures the in-flight BRAM write value and feeds it back to the read stage if the next message targets the same address. Eliminates the RAW hazard without moving BRAM off the hot path. More complex than eviction (requires address comparison on every cycle) and still pays 2-cycle BRAM read latency unconditionally. Rejected.

**Larger register file (N > 8):**
Wider comparator trees, more flip-flops, longer combinatorial depth. 8 levels covers the near-touch adequately for spread-capture and risk monitoring. Can be changed at compile time — N is a template parameter.

