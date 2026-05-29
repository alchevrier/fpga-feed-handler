# ADR-009 — Interface Partitioning: `s_axilite` for Configuration, `ap_fifo` for the Hot Path

## Status
Accepted

## Date
2026-05-29

## Context

The kernel requires two categories of input:

1. **Configuration parameters** — `base_price` and `tick_size`, which are fixed per-instrument constants used to compute the BRAM address (ADR-005). These are known before the feed starts and do not change during operation.
2. **Market data** — a stream of ITCH Add Order messages arriving at line rate during operation.

These two categories have fundamentally different timing, latency, and throughput requirements:

| | Configuration | Market data |
|---|---|---|
| Source | Host processor / management plane | ITCH feed (NIC or pcap) |
| Timing | Once at startup, before feed starts | Continuous, at line rate |
| Latency requirement | None — off hot-path | Deterministic, 6 cycles |
| Interface | Register write | Stream |

Mixing them on the same interface would either require the kernel to implement a state machine to distinguish initialisation messages from hot-path messages (adding cycles and complexity to the hot path), or would require the feed to carry configuration framing that is not part of the ITCH protocol.

## Decision

The kernel exposes two completely independent interfaces:

```cpp
void feed_handler_kernel(
    hls::stream<ap_uint<8>>& in,          // #pragma HLS INTERFACE ap_fifo — hot path (non-const: FIFO pop is a mutation)
    BookSnapshot&             snap,        // #pragma HLS INTERFACE ap_memory — output
    const ap_uint<32>         base_price,  // #pragma HLS INTERFACE s_axilite
    const ap_uint<32>         inv_tick     // #pragma HLS INTERFACE s_axilite — precomputed: (1u << 16) / tick_size
);
```

**`s_axilite` — configuration plane (off hot-path):**
- `base_price` and `inv_tick` are mapped to AXI4-Lite slave registers.
- The host writes these registers once before starting the kernel (via PCIe, Zynq PS, or a management CPU).
- The host is responsible for deriving `base_price` and `tick_size` — **neither is transmitted by NASDAQ in the ITCH Stock Directory (`R`) message**. `tick_size` is determined by exchange rules (NMS stocks ≥ $1.00 → tick = $0.01 = 100 in ITCH fixed-point). `base_price` is chosen by the operator (prior close or minimum expected price for the session, rounded down to the nearest tick). The host then computes `inv_tick = (1u << 16) / tick_size` and writes both values via `s_axilite`.
- After the kernel is started, these values are read as constants by `parse_message`. They never change during operation. Passing `inv_tick` rather than `tick_size` means the kernel never divides — the hot path uses multiply + shift only (ADR-005).
- AXI4-Lite register reads have no timing relationship to the hot-path clock cycles — they are sampled once and held in registers by HLS.

**`ap_fifo` — data plane (hot path):**
- Raw ITCH `A` message bytes arrive on the stream at line rate.
- The kernel never processes `R` messages — that responsibility belongs to the host.
- The hot path is purely: read bytes → extract fields → compute index → read-modify-write BRAM → update snapshot.
- No branching on message type, no mode check, no initialisation logic in the pipeline.

## Why This Is the Right Partitioning

Putting initialisation on the `s_axilite` interface rather than in the kernel's data stream eliminates an entire class of design problems:

- **No state machine in the kernel.** There is no "waiting for `R` message" mode, no transition logic, no guard on whether `base_price` is valid. The kernel starts in a single, well-defined mode: process `A` messages.
- **No hot-path cost.** A kernel-internal init state machine would require a mode register and a conditional branch on every message — directly on the critical path. `s_axilite` moves that entirely off-chip to the host.
- **Clean separation of concerns.** The kernel owns the 6-cycle hot-path datapath. The host owns protocol framing, instrument lookup, and configuration delivery. These are the right responsibilities for each.
- **Standard Vitis HLS pattern.** `s_axilite` is the canonical interface for kernel control registers in Vitis HLS. Synthesis, simulation, and driver generation are all first-class supported.

## Consequences

- The kernel cannot be started until the host has written valid `base_price` and `inv_tick` values. This is an operational pre-condition, not a kernel concern.
- If the instrument changes (e.g., the kernel is reused for a different symbol), the host must stop the kernel, compute new `base_price` and `inv_tick` from the new instrument's `R` message, and restart.
- The `s_axilite` interface adds a top-level AXI4-Lite slave port to the synthesised IP block, which is standard in any Vivado IP integration flow.
- The host-side responsibility for processing the ITCH `R` message is explicitly out of scope for the HLS kernel. It belongs in the management software layer.
