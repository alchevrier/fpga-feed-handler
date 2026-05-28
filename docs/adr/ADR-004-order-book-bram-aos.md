# ADR-004 — Order Book Storage: AOS in BRAM

## Status
Accepted

## Date
2026-05-28

## Context

The C++ reference implementation (`low-latency-feed-handler`) uses a **Structure of Arrays (SOA)** layout for the order book:

```cpp
// SOA — separate arrays per field
alignas(64) std::array<Price, MAX_LEVELS> bid_prices;
alignas(64) std::array<Qty,   MAX_LEVELS> bid_qtys;
alignas(64) std::array<Price, MAX_LEVELS> ask_prices;
alignas(64) std::array<Qty,   MAX_LEVELS> ask_qtys;
```

SOA is optimal on a CPU because:
- It enables SIMD vectorisation over a single field (e.g., scanning all prices in one AVX-512 pass).
- It keeps field-homogeneous cache lines hot during batch operations.

On an FPGA the access pattern is fundamentally different. There is no SIMD. The hot path is a **single indexed read-modify-write at one price level per message**. We do not scan across levels — we jump directly to the level by index (see ADR-005).

For a single indexed access, SOA requires **two separate BRAM reads** — one for the price array, one for the qty array — consuming two BRAM ports and two cycles of read latency per field. An Array of Structures (AOS) packs both fields into a single BRAM word, so the entire `{price, qty}` record is retrieved in **one read at one address**, consuming one BRAM port.

## Decision

The order book is stored in BRAM using an **Array of Structures (AOS)** layout:

```cpp
struct Level {
    ap_uint<32> price;
    ap_uint<32> qty;
};

// AOS — one BRAM array, one address per level
Level bids[MAX_LEVELS];  // #pragma HLS BIND_STORAGE variable=bids type=ram_2p
Level asks[MAX_LEVELS];
```

Each BRAM address holds a complete `{price, qty}` record. A single read retrieves everything needed to compute the updated level. A single write commits the result.

The side (bid/ask) is encoded as the most-significant address bit:

```cpp
// addr = side_bit | level_index  — no branch, just bit composition
uint16_t addr = (side << BOOK_ADDR_WIDTH) | level_index;
```

This allows bids and asks to share a single BRAM array if desired, or remain as two separate arrays. Either way, no branch is needed to select the target array.

## Alternatives Considered

**SOA in BRAM**: two arrays per side (price array, qty array). Requires two BRAM reads per access (2 cycles each, potentially serialised due to BRAM port constraints). No SIMD benefit exists on FPGA. Rejected.

**LUTRAM (distributed RAM)**: lower latency than BRAM (1 cycle) but consumes LUT resources that are scarcer than BRAM on UltraScale+ for large arrays. Acceptable for small arrays (e.g., top-N snapshot); not appropriate for the full book. Rejected for the main book storage.

**UltraRAM (URAM)**: higher density than BRAM, 3-cycle read latency. Would increase `handle_event` stage latency from 4 cycles to 6 cycles, pushing total pipeline latency to 8 cycles and risking the frequency target. Rejected.

## Consequences

- BRAM `ram_2p` (simple dual-port) gives one read port and one write port per clock. The read in cycle 1-2 and the write in cycle 4 are non-overlapping, so no port conflict within `handle_event`.
- The snapshot registers (best bid/ask) must NOT share the same BRAM — they are in UltraFast registers to avoid port conflicts (see ADR-006).
- `MAX_LEVELS` must be chosen at synthesis time. It bounds the price range the book can represent: `MAX_LEVELS = (max_price - min_price) / tick_size`. This is a known constant for a given instrument from the ITCH Stock Directory (`R` message).
- AOS layout means HLS may pad the struct to meet BRAM word-width alignment. Explicit `ap_uint` field widths prevent unexpected padding.
