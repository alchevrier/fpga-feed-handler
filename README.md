# fpga-feed-handler

FPGA NASDAQ ITCH 5.0 feed handler implemented in Vitis HLS. Targeted 6-cycle deterministic parse-to-book-insert pipeline at 250 MHz (24 ns, pre-synthesis design target). Benchmarked against a C++ software baseline of 274 ns P99.9.

## Motivation

The C++ reference implementation ([low-latency-feed-handler](https://github.com/alchevrier/low-latency-feed-handler)) achieves **274 ns P99.9** parse-to-book-insert latency on a real NASDAQ ITCH 5.0 dataset (8,669 symbols, DRAM tier). That figure represents the ceiling of what the application code alone can achieve: zero-allocation pipeline, lock-free MPSC queue, cache-line-aligned SOA arrays, dedicated isolated cores, suppressed OS tick. At the code level, the only remaining avenue is eliminating a `memmove` when inserting a new price level into the SOA order book — single-digit nanoseconds at best. Further reductions are possible through system-level tuning (BIOS power management, NUMA pinning, IRQ affinity, hugepage configuration, kernel build flags), but those close the gap by tens of nanoseconds at best. The remaining latency is not a code problem.

The remaining latency is structural: cache misses with data-dependent latency, branch mispredictions in binary search over price levels, MESI coherency traffic between threads, and kernel interference that cannot be fully evicted from an isolated core — scheduler residuals, non-moveable IRQs, and RCU callbacks that fire regardless of `isolcpus`, `nohz_full`, and `rcu_nocbs`. None of these can be eliminated in software.

An FPGA removes all three sources:
- **BRAM latency is fixed at 2 cycles every time** — no cache hierarchy, no miss penalty.
- **No branch predictor** — conditional logic synthesises to combinatorial mux trees with deterministic cycle cost.
- **No OS, no coherency protocol** — bare-metal datapath. If the 6-cycle design target is confirmed by HLS synthesis, there is no variable-latency path: P50 = P99 = P99.9 = P99.99 = P100 = 6 cycles.

## Architecture

```
ap_fifo (ITCH payload bytes)
    │
    ▼
┌─────────────────┐   hls::stream<MarketEvent>   ┌──────────────────┐   hls::stream<LevelUpdate>   ┌───────────────────┐
│  parse_message  │ ────────────────────────────▶ │  handle_event    │ ──────────────────────────▶  │ update_snapshot   │
│  1 cycle, II=1  │                               │  4 cycles, II=1  │                              │  1 cycle, II=1    │
└─────────────────┘                               └──────────────────┘                              └───────────────────┘
                                                          │                                                   │
                                                    AOS BRAM book                                   UltraFast registers
                                                  (2-cycle read latency)                            (clock-edge atomic)
```

**Design target: 6 cycles @ 250 MHz = 24 ns** (pending HLS synthesis confirmation).
**Pipeline II target: 1** — one new message accepted per clock cycle.

### Key design decisions

**Mathematical BRAM addressing (ADR-005)**
Price level lookup uses `idx = (price - base_price) / tick_size` — pure combinatorial logic, no loop, no branch, no loop-carried dependency. This is what makes II=1 achievable: binary search over price levels would require O(log N) loop iterations with data-dependent termination, making II > 1 and latency non-deterministic.

**AOS in BRAM (ADR-004)**
Each BRAM address holds a complete `{price, qty}` record. A single read retrieves everything needed to compute the updated level. The C++ reference uses SOA for SIMD vectorisation; there is no SIMD on an FPGA and the access pattern is single-index, so SOA's two-array read cost is strictly worse.

**DATAFLOW pipeline with `hls::stream` (ADR-003)**
Three tasks connected by HLS streams with `#pragma HLS DATAFLOW`. The 4-cycle `handle_event` stage is internally pipelined, so the top-level II is 1 despite the stage latency. Tasks execute concurrently on successive messages.

**UltraFast registers + clock-edge atomicity for snapshot (ADR-006)**
The best bid/ask snapshot is in flip-flop registers, not BRAM. BRAM ports are fully occupied by `handle_event`; a second reader would require arbitration and stall the pipeline. On FPGA, all flip-flop outputs are stable for the full clock period and captured simultaneously on the rising edge — a reader in any clock cycle sees all four fields from the same clock edge, atomically. No seqlock, no handshake, no retry: the clock is the synchronisation primitive.

**`ap_fifo` input at ITCH payload boundary (ADR-002, ADR-009)**
The kernel ingests raw ITCH bytes on the `ap_fifo` data plane. Configuration parameters (`base_price`, `tick_size`) are supplied once at startup by the host via `s_axilite` register writes — completely off the hot path. The kernel never processes ITCH `R` messages; the host extracts the constants and writes them before starting the feed. No state machine, no mode check, no init logic in the pipeline.

**HLS as design verifier (ADR-007)**
HLS synthesis reports confirm II, latency, resource utilisation, and timing closure at 250 MHz. These are the questions a C++ functional model cannot answer. RTL co-simulation provides cycle-accurate verification without a hand-written SystemVerilog testbench.

**Add Order only in this phase (ADR-008)**
Cancel, execute, and delete message types exercise the same BRAM read-modify-write path with different arithmetic. They are deferred until the synthesis report confirms the target II and latency for the Add path.

## Benchmark

> **Numbers are order-of-magnitude indicators, not precise measurements.** The C++ figures are from a specific machine (Intel i5-12400) under specific conditions; production hardware, NUMA topology, and kernel configuration will shift them. The FPGA figure is derived from an HLS synthesis cycle count at a target frequency — actual silicon latency depends on place-and-route, clock distribution, and board parasitics.

| Metric | C++ baseline | FPGA (this project) |
|---|---|---|
| P50 latency | ~20 ns | 24 ns (6 cycles @ 250 MHz) — design target |
| P99.9 latency | 274 ns | 24 ns — design target |
| Latency distribution | Variable — P50 ≪ P99.9 | Fixed — P50 = P99.9 = P100 (if target confirmed) |
| Dataset | 8,669 symbols, real ITCH 5.0 | Single instrument, synthetic |
| Measurement | TSC per-message (real ITCH file) | HLS design target (synthesis not yet run) |

The C++ P50 of ~20 ns reflects the market's power law: the top symbols (AAPL, MSFT, SPY, …) generate the overwhelming majority of messages and their order books stay L1-resident. At the median, the C++ baseline is comparable to the FPGA target.

The C++ P99.9 of 274 ns reflects illiquid symbols — books that have not been touched for seconds, evicted from L3, requiring a DRAM fetch (~80 ns round trip) on top of the compute cost. The FPGA has no cache hierarchy: every symbol, liquid or not, costs the same 2-cycle BRAM access. The FPGA does not improve median latency — it eliminates the tail.

## Architecture Decision Records

Full rationale for each design decision is in [`docs/adr/`](docs/adr/):

- [ADR-001](docs/adr/ADR-001-why-fpga.md) — Why FPGA
- [ADR-002](docs/adr/ADR-002-input-abstraction-ap-fifo.md) — Input abstraction: `ap_fifo` at ITCH payload boundary
- [ADR-003](docs/adr/ADR-003-dataflow-pipeline-decomposition.md) — DATAFLOW pipeline decomposition
- [ADR-004](docs/adr/ADR-004-order-book-bram-aos.md) — Order book storage: AOS in BRAM
- [ADR-005](docs/adr/ADR-005-mathematical-index-deterministic-addressing.md) — Deterministic BRAM addressing: mathematical index over binary search
- [ADR-006](docs/adr/ADR-006-snapshot-registers-seqlock.md) — Snapshot registers: UltraFast registers, clock-edge atomicity
- [ADR-007](docs/adr/ADR-007-hls-as-design-verifier.md) — HLS as design verifier
- [ADR-008](docs/adr/ADR-008-add-only-scope.md) — Scope: ADD Order events only
- [ADR-009](docs/adr/ADR-009-interface-partitioning-s-axilite-ap-fifo.md) — Interface partitioning: `s_axilite` for configuration, `ap_fifo` for the hot path

## Reference Implementation

[low-latency-feed-handler](https://github.com/alchevrier/low-latency-feed-handler) — NASDAQ ITCH 5.0 parser, MPSC queue, SOA order book, DPDK pcap PMD pipeline. The C++ baseline this project benchmarks against.
