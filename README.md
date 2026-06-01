# fpga-feed-handler

FPGA NASDAQ ITCH 5.0 feed handler implemented in Vitis HLS. RTL co-simulation verified: **13-cycle deterministic arbitrate-parse-book-insert pipeline at 250 MHz (52 ns)**. Benchmarked against a C++ software baseline of 274 ns P99.9 — a 5.3× reduction in tail latency with a fixed, distribution-free result.

## Motivation

The C++ reference implementation ([low-latency-feed-handler](https://github.com/alchevrier/low-latency-feed-handler)) achieves **274 ns P99.9** parse-to-book-insert latency on a real NASDAQ ITCH 5.0 dataset (8,669 symbols, DRAM tier). That figure represents the ceiling of what the application code alone can achieve: zero-allocation pipeline, lock-free MPSC queue, cache-line-aligned SOA arrays, dedicated isolated cores, suppressed OS tick. At the code level, the only remaining avenue is eliminating a `memmove` when inserting a new price level into the SOA order book — single-digit nanoseconds at best. Further reductions are possible through system-level tuning (BIOS power management, NUMA pinning, IRQ affinity, hugepage configuration, kernel build flags), but those close the gap by tens of nanoseconds at best. The remaining latency is not a code problem.

The remaining latency is structural: cache misses with data-dependent latency, branch mispredictions in binary search over price levels, MESI coherency traffic between threads, and kernel interference that cannot be fully evicted from an isolated core — scheduler residuals, non-moveable IRQs, and RCU callbacks that fire regardless of `isolcpus`, `nohz_full`, and `rcu_nocbs`. None of these can be eliminated in software.

An FPGA removes all three sources:
- **BRAM latency is fixed at 2 cycles every time** — no cache hierarchy, no miss penalty.
- **No branch predictor** — conditional logic synthesises to combinatorial mux trees with deterministic cycle cost.
- **No OS, no coherency protocol** — bare-metal datapath. RTL co-simulation confirms a 9-cycle latency: there is no variable-latency path — P50 = P99 = P99.9 = P99.99 = P100 = 9 cycles.

## Architecture

```
 ap_fifo feed_a   ap_fifo feed_b
        │                │
        └───────┬─────────┘
                ▼
   ┌─────────────────────┐
   │      arbitrate      │
   │   1 cycle, II=1     │
   │  (signal mux,       │
   │   seq check,        │
   │   pre-book)         │
   └──────────┬──────────┘
              │ hls::stream<ap_uint<288>>
              ▼
   ┌─────────────────────┐
   │   parse_add_event   │
   │   7 cycles, II=1    │
   │  (DSP multiply      │
   │   absorbed by II=1  │
   │   pipeline)         │
   └──────────┬──────────┘
              │ hls::stream<MarketEvent>
              ▼
   ┌─────────────────────┐
   │    handle_event     │
   │   4 cycles, II=1    │
   │  (BRAM r/w,         │
   │   no multiply)      │
   └──────────┬──────────┘
              │ hls::stream<LevelUpdate>
              ▼
   ┌─────────────────────┐
   │   update_snapshot   │
   │   0 cycles, II=1    │
   │  (register          │
   │   compare/write)    │
   └──────┬──────────────┘
          │              │
   AOS BRAM book    UltraFast registers
 (2-cycle latency)  (clock-edge atomic)
```

**CoSim-verified: 9 cycles @ 250 MHz = 36 ns.** Pipeline II=1 — one new message accepted per clock cycle.

The DSP multiply (`price × inv_tick`) for index computation costs 7 cycles of pipeline depth in `parse_add_event`, but since that stage is `II=1`, its latency is fully hidden — a new message enters every cycle while previous messages flow through the DSP pipeline.

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

**Multi-feed arbitration at the signal level (ADR-010)**
With two `ap_fifo` inputs, a dedicated `arbitrate` stage selects the winning message by sequence number or priority **before** `parse_add_event` is invoked — a 1-cycle combinatorial mux. On a CPU MPSC architecture, each feed is a producer thread and the validity check runs inside the book writer, after the message has already traversed the queue. The book can therefore hold an incorrect state for the duration of the starvation guard expiry on the correcting feed — an indeterminate window during which any generated order is based on wrong prices, constituting a MiFID II best execution breach. On FPGA, the book is never written before arbitration completes: the violation window has zero duration by construction. This makes FPGA arbitration a compliance requirement first and a performance advantage second.

## Benchmark

> **Numbers are order-of-magnitude indicators, not precise measurements.** The C++ figures are from a specific machine (Intel i5-12400) under specific conditions; production hardware, NUMA topology, and kernel configuration will shift them. The FPGA figure is derived from an HLS synthesis cycle count at a target frequency — actual silicon latency depends on place-and-route, clock distribution, and board parasitics.

| Metric | C++ baseline | FPGA (initial design target) | FPGA (CoSim verified, single feed) | FPGA (CoSim verified, dual feed) |
|---|---|---|---|---|
| P50 latency | ~20 ns | 24 ns (6 cycles @ 250 MHz) | 36 ns (9 cycles @ 250 MHz) | **52 ns (13 cycles @ 250 MHz)** |
| P99.9 latency | 274 ns | 24 ns | 36 ns | **52 ns** |
| Latency distribution | Variable — P50 ≪ P99.9 | Fixed (if target confirmed) | Fixed — P50 = P99.9 = P100 | **Fixed — P50 = P99.9 = P100 (RTL confirmed)** |
| Dataset | 8,669 symbols, real ITCH 5.0 | Single instrument, synthetic | Single instrument, synthetic | Single instrument, synthetic |
| Measurement | TSC per-message (real ITCH file) | HLS design target (pre-synthesis) | RTL co-simulation (xsim, Verilog) | RTL co-simulation (xsim, Verilog) |

The C++ P50 of ~20 ns reflects the market's power law: the top symbols (AAPL, MSFT, SPY, …) generate the overwhelming majority of messages and their order books stay L1-resident. At the median, the C++ baseline is comparable to the FPGA target.

The C++ P99.9 of 274 ns reflects illiquid symbols — books that have not been touched for seconds, evicted from L3, requiring a DRAM fetch (~80 ns round trip) on top of the compute cost. The FPGA has no cache hierarchy: every symbol, liquid or not, costs the same 2-cycle BRAM access. The FPGA does not improve median latency — it eliminates the tail.

## Synthesis Results and Reports

Full Vitis HLS synthesis reports are available in the repository at:

  docs/reports/

Key results (synthesis + RTL co-simulation):

- **Target device:** xa7a12t-cpg238-2I
- **Tool version:** Vitis HLS 2025.2
- **Clock period:** 4.00 ns (250 MHz)
- **Kernel latency (CoSim, RTL):** 13 cycles (52 ns) ✅
- **Kernel II (CoSim, RTL):** 16 cycles
- **CoSim status:** PASS (Verilog/xsim)
- **Per-stage breakdown:**
    - `arbitrate`: 3 cycles, II=1 (static 64-bit seq counter: compare latency=1 + add latency=1 are the bottleneck)
    - `parse_add_event`: 7 cycles, II=1 (DSP multiply, latency absorbed)
    - `handle_event`: 4 cycles (BRAM read-modify-write, no DSP)
    - `update_snapshot`: 0 cycles, II=1 (register compare/write)
- **Resource utilization:**
    - BRAM_18K: 1 (2%)
    - DSP: 2 (5%)
    - FF: 3693 (23%)
    - LUT: 1657 (20%)

For detailed breakdowns (including per-function reports), see:

  docs/reports/kernel_csynth.rpt
  docs/reports/handle_event_csynth.rpt
  docs/reports/parse_add_event_csynth.rpt
  docs/reports/update_snapshot_csynth.rpt

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
- [ADR-010](docs/adr/ADR-010-multi-feed-arbitration-signal-vs-mpsc.md) — Multi-feed arbitration: hardware signal mux vs MPSC post-hoc validation
- [ADR-011](docs/adr/ADR-011-single-vs-dual-feed-bitstream.md) — Single-feed vs dual-feed bitstream: arbitration cost +4 cycles / +16 ns (CoSim verified)

## Reference Implementation

[low-latency-feed-handler](https://github.com/alchevrier/low-latency-feed-handler) — NASDAQ ITCH 5.0 parser, MPSC queue, SOA order book, DPDK pcap PMD pipeline. The C++ baseline this project benchmarks against.
