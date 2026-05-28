# ADR-002 — Input Abstraction: `ap_fifo` at the ITCH Payload Boundary

## Status
Accepted

## Date
2026-05-28

## Context

A production FPGA feed handler would receive Ethernet frames from a NIC, strip the UDP/IP/Ethernet headers, and forward the ITCH payload bytes to the parser. This is the full pipeline:

```
NIC → Ethernet frame → UDP demux → ITCH payload → parser → order book
```

Implementing Ethernet/UDP parsing in HLS is a well-understood but non-trivial engineering task. It introduces:

- Frame alignment handling (multi-beat AXI4-Stream ingestion)
- UDP checksum validation logic
- Protocol-layer concerns unrelated to the core design problem

The core design problem being explored in this project is the **ITCH parse-to-book-insert pipeline**: can a deterministic 6-cycle latency be achieved? That question does not require a live NIC or a UDP stack to answer.

## Decision

The kernel's input interface is modelled as an `ap_fifo` (or `hls::stream`) carrying raw ITCH payload bytes. The caller is responsible for delivering correctly framed ITCH messages to the FIFO.

```cpp
void feed_handler_kernel(hls::stream<ap_uint<8>>& in, BookSnapshot& snapshot);
```

This is equivalent to entering the pipeline at the point immediately after UDP stripping — the same boundary used by the C++ reference implementation's DPDK path, where `rte_pktmbuf_mtod()` returns a pointer to the ITCH payload after the Ethernet/UDP headers have already been consumed by the PMD.

For simulation and HLS co-simulation, the testbench feeds pre-captured ITCH messages directly into the stream. This is structurally identical to the C++ benchmark's approach of parsing a raw ITCH binary file.

## Alternatives Considered

**Full Ethernet/UDP parsing in HLS**: rejected for this phase. Adds protocol complexity without advancing the core latency question. Ethernet/UDP framing is a separable concern that can be handled by a dedicated AXI4-Stream IP (e.g., the Xilinx UDP offload engine) placed upstream of this kernel.

**AXI4-Stream with TKEEP/TLAST signals**: correct interface for production integration with an AXI4-Stream UDP engine. Deferred — not needed for HLS verification of the parse-to-book pipeline.

## Consequences

- The kernel interface is simple and directly testable from a C++ testbench without hardware.
- Integration with a full network stack requires wrapping this kernel with an AXI4-Stream adapter, which is a standard pattern.
- The scope boundary is explicit: this kernel owns ITCH parsing and book update; it does not own network framing.
