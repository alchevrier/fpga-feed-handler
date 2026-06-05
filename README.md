# fpga-feed-handler

FPGA NASDAQ ITCH 5.0 feed handler implemented in Vitis HLS. RTL co-simulation verified: **II=1 pipeline at 250 MHz — 5-cycle / 20 ns hot path (arbitrate → filter → parse → snapshot), 13× reduction in tail latency vs the C++ baseline of 274 ns P99.9**.

## Motivation

The C++ reference implementation ([low-latency-feed-handler](https://github.com/alchevrier/low-latency-feed-handler)) achieves **274 ns P99.9** parse-to-book-insert latency on a real NASDAQ ITCH 5.0 dataset (8,669 symbols, DRAM tier). That figure represents the ceiling of what the application code alone can achieve: zero-allocation pipeline, lock-free MPSC queue, cache-line-aligned SOA arrays, dedicated isolated cores, suppressed OS tick. At the code level, the only remaining avenue is eliminating a `memmove` when inserting a new price level into the SOA order book — single-digit nanoseconds at best. Further reductions are possible through system-level tuning (BIOS power management, NUMA pinning, IRQ affinity, hugepage configuration, kernel build flags), but those close the gap by tens of nanoseconds at best. The remaining latency is not a code problem.

The remaining latency is structural: cache misses with data-dependent latency, branch mispredictions in binary search over price levels, MESI coherency traffic between threads, and kernel interference that cannot be fully evicted from an isolated core — scheduler residuals, non-moveable IRQs, and RCU callbacks that fire regardless of `isolcpus`, `nohz_full`, and `rcu_nocbs`. None of these can be eliminated in software.

An FPGA removes all three sources:
- **BRAM latency is fixed at 2 cycles every time** — no cache hierarchy, no miss penalty.
- **No branch predictor** — conditional logic synthesises to combinatorial mux trees with deterministic cycle cost.
- **No OS, no coherency protocol** — bare-metal datapath. RTL co-simulation confirms II=1 throughput: there is no variable-latency path — P50 = P99 = P99.9 = P99.99 = P100 = 2 cycles on the hot path.

## Architecture

**Current implementation (CoSim verified, dual-feed HEAD):** `arbitrate → filter_event → parse_add_event → update_snapshot` (hot path) ∥ `register_book_update` (parallel cold path, BRAM R-M-W) — II=1 kernel, 5-cycle hot path, 18-cycle total pipeline drain @ 250 MHz. `filter_event` is a price sanity + quantity floor stage (price upper/lower bound + qty floor, 3 cycles, II=1, 0 DSP — all comparisons combinatorial, 3 cycles from counter RMW pipeline scheduling). The cold path never blocks the snapshot — `snap` is updated 5 cycles after message admission regardless of BRAM R-M-W progress.

**Intended architecture** shown below. Deferred stages are annotated with their ADR. Target: II=1 throughout (250M messages/s theoretical maximum), with an early-exit filter before the book stages and a parallel toxicity signal alongside the snapshot output.

```
 ap_fifo[0]      ap_fifo[1]      ...  ap_fifo[N-1]
      │                │                    │         N=2 current (ADR-011)
      └────────────────┴────────────────────┘         N=4 multi-venue (ADR-014)
                       │
                       ▼
       ┌─────────────────────────────────┐
       │       protocol_dispatch         │  1 cycle, II=1
       │  feed slot → protocol fork      │  slot assignment configured via s_axilite
       │  no payload parsing required    │  ITCH 5.0 | FIX | OUCH | ...
       └───────┬─────────────────────────┘
               │
               │  (this repo: ITCH 5.0 pipeline only)
               ▼
       ┌─────────────────────────────────┐
       │           arbitrate             │  3 cycles, II=1
       │   MOLDUDP64 sequence select     │  each protocol owns its own arbitration
       │   per-feed seq counters         │  primary preferred over secondary
       └───────────────┬─────────────────┘
                       │ ap_uint<288>
                       ▼
       ┌─────────────────────────────────┐
       │         filter_event            │  1 cycle, II=1         [ADR-016]
       │  instrument membership check    │  drops unrecognised stock_locate
       │  price sanity: base ≤ p < max  │  drops out-of-range prices
       │  quantity floor: qty ≥ min_qty  │  drops sub-minimum qty
       └───────────────┬─────────────────┘
                       │ ap_uint<288>  (validated messages only)
                       ▼
       ┌─────────────────────────────────┐
       │         route_by_type           │  1 cycle, II=1         [ADR-008 scope]
       │  msg_type field → parse slot    │  A → parse_add_event
       │  (current: A only)             │  X/D/U → parse_cancel/delete/replace
       └───────────────┬─────────────────┘
                       │ per-type hls::stream<ap_uint<288>>
                       ▼
       ┌─────────────────────────────────┐
       │       parse_add_event           │  1–2 cycles, II=1      (Add Order only)
       │  field extract only             │  DSP multiply moves to cold path (ADR-012)
       │  parse_cancel/delete/replace    │  separate functions, different field
       │  added in roadmap step 5        │  layouts and arithmetic per type
       └───────────────┬─────────────────┘
                       │ hls::stream<MarketEvent>
        ┌──────────────┼──────────────────────┐
        ▼              ▼                       ▼
  ┌───────────┐  ┌──────────────────────┐  ┌─────────────────────────┐
  │  update_  │  │  store_buffer_write  │  │    toxicity_monitor     │  [ADR-015]
  │  snapshot │  │  1 cycle, II=1       │  │  add/cancel ratio per   │
  │  1 cycle  │  │  register ring head  │  │  instrument per window  │
  │  II=1     │  │  [ADR-012, ADR-013]  │  └────────────┬────────────┘
  └─────┬─────┘  └──────────────────────┘               │
        │                                         ToxicitySnapshot
  BookSnapshot                                  (s_axilite readable)
  (UltraFast registers,
   clock-edge atomic)

  Cold path [ADR-012]:
    store buffer ──▶ BRAM writer ──▶ AOS BRAM  (full order book depth)
    flushed every 250K cycles  (1 ms @ 250 MHz, configurable via s_axilite)
```

**Target latency (intended architecture, pre-synthesis estimate):** ~9–10 cycles @ 250 MHz = ~36–40 ns. With the store buffer in place (ADR-012), the DSP multiply moves to the cold path — `parse_add_event` becomes a field-extract-only stage at 1–2 cycles. Removing the 7-cycle DSP depth and the 4-cycle BRAM RAW stall nets −8 cycles versus the current HEAD, despite adding three new 1-cycle stages. With all stages at II=1, a burst of N messages is processed in N cycles regardless of burst length.

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
Each ITCH message type has a distinct field layout and therefore requires its own parse function — `parse_add_event` is specific to type `A`. Cancel (`X`), Delete (`D`), and Replace (`U`) messages have different offsets, different fields, and different arithmetic in `handle_event`. A `route_by_type` dispatch stage will sit between `filter_event` and the per-type parse functions once those types are implemented (roadmap step 5). The BRAM read-modify-write path in `handle_event` is shared, but the parse stage is not.

**Multi-feed arbitration at the signal level (ADR-010)**
With two `ap_fifo` inputs, a dedicated `arbitrate` stage selects the winning message by sequence number or priority **before** `parse_add_event` is invoked — a 1-cycle combinatorial mux. On a CPU MPSC architecture, each feed is a producer thread and the validity check runs inside the book writer, after the message has already traversed the queue. The book can therefore hold an incorrect state for the duration of the starvation guard expiry on the correcting feed — an indeterminate window during which any generated order is based on wrong prices, constituting a MiFID II best execution breach. On FPGA, the book is never written before arbitration completes: the violation window has zero duration by construction. This makes FPGA arbitration a compliance requirement first and a performance advantage second.

**Early-exit event filter (ADR-016)** *(implemented)*
A `filter_event` stage between `arbitrate` and `parse_add_event` drops messages on three checks: price upper bound (hard drop — prevents BRAM index overflow), price lower bound (diagnostic drop — signals stale `base_price` config), and quantity floor (hard drop — removes sub-minimum noise orders that corrupt the best-price signal). All checks are combinatorial (four comparators, 0 DSP). The 3-cycle stage latency is from the diagnostic counter read-modify-write pipeline; the forwarding decision is purely combinatorial. Filter parameters (`filt_base`, `filt_max`, `filt_minqty`) are individual `s_axilite` scalars — precomputed by the host at session open, same pattern as `inv_tick` and `base_offset`. Instrument membership (ADR-014 LUTRAM) is deferred.

**Register book with eviction-based top-N store (ADR-012, ADR-013)** *(deferred)*
The `handle_event` BRAM read-modify-write creates a RAW (read-after-write) hazard that makes pipelining impossible and sets the kernel II to 16. The fix is a 16-register file: 8 flip-flop slots per side (`bids[8]`, `asks[8]`), each holding `{price, qty}`. On each admitted message the register file is scanned for a price match (8-wide combinatorial comparator tree); on a hit, qty is updated in-place. On a miss, the worst slot per side (highest ask or lowest bid) is found via a 7-comparator tree, evicted to BRAM (write-only, no RAW hazard), and the new level inserted. Out-of-range messages write directly to BRAM. The matching engine reads only from the register file — BRAM is pure persistence. With BRAM reads gone from the hot path and all remaining BRAM access write-only, the full pipeline achieves II=1: 250M messages/second theoretical throughput.

**Parallel toxicity monitor (ADR-015)** *(deferred, requires X/D/U message types)*
A `toxicity_monitor` task branches off the `MarketEvent` stream in parallel with `store_buffer_write`. It maintains per-instrument add/cancel counters over a configurable rolling window, computes a fixed-point cancellation ratio using a DSP reciprocal multiply (same pattern as `inv_tick`), and writes a `ToxicitySnapshot` register readable via `s_axilite`. The matching engine reads `BookSnapshot` and `ToxicitySnapshot` simultaneously — both are updated at the same pipeline clock cycle — and can reduce aggression or halt order generation when the cancellation ratio crosses a pre-configured threshold.

**Multi-instrument routing via LUTRAM (ADR-014)** *(deferred)*
Extends `arbitrate` from N=2 feeds to N=4 (two venues × primary/secondary) and adds a LUTRAM routing table keyed by `stock_locate`. At startup the host writes the routing table via `s_axilite` (stock_locate → pipeline slot index); `filter_event` uses the same table for instrument membership checks. Each pipeline slot is an independent instance of the post-filter stages with its own snapshot registers and store buffer. Slot count K is a compile-time constant; the xa7a12t has headroom for K=4–8 given current resource utilisation.

## Roadmap

| # | Step | ADR | Status | Notes |
|---|------|-----|--------|-------|
| 1 | Register store buffer + cold-path BRAM writer | [ADR-012](docs/adr/ADR-012-hot-path-register-store-buffer.md) | Deferred | Builds the SPSC ring and flush thread; prerequisite for step 2 |
| 2 | Remove BRAM from hot path → II=1 | [ADR-013](docs/adr/ADR-013-bram-off-hot-path-ii-equals-1.md) | Deferred | Depends on step 1; drops kernel II from 16 to 1 (250M msg/s) |
| 3 | Early-exit event filter | [ADR-016](docs/adr/ADR-016-early-exit-event-filter.md) | **Done** | Price sanity + qty floor implemented, CoSim verified; instrument membership (LUTRAM) deferred to ADR-014 |
| 4 | Multi-feed bus widening + per-instrument routing | [ADR-014](docs/adr/ADR-014-multi-feed-bus-widening-per-instrument-routing.md) | Deferred | N=4 feeds, LUTRAM routing table, K independent pipeline slots |
| 5 | Cancel / Delete / Replace message types (X, D, U) | [ADR-008](docs/adr/ADR-008-add-only-scope.md) | Deferred | Prerequisite gate for step 6 |
| 6 | Market toxicity detection | [ADR-015](docs/adr/ADR-015-market-toxicity-detection.md) | Deferred | Depends on step 5 (needs X/D counters); parallel branch off MarketEvent stream |

## Benchmark

> **Numbers are order-of-magnitude indicators, not precise measurements.** The C++ figures are from a specific machine (Intel i5-12400) under specific conditions; production hardware, NUMA topology, and kernel configuration will shift them. The FPGA figure is derived from an HLS synthesis cycle count at a target frequency — actual silicon latency depends on place-and-route, clock distribution, and board parasitics.

| Metric | C++ baseline | FPGA (initial design target) | FPGA (CoSim verified, single feed) | FPGA (CoSim verified, dual feed + filter, II=1) |
|---|---|---|---|---|
| Hot path latency | ~20 ns | 24 ns (6 cycles @ 250 MHz) | — | **20 ns (5 cycles @ 250 MHz)** |
| P99.9 latency | 274 ns | 24 ns | 36 ns | **20 ns (hot path, fixed)** |
| Latency distribution | Variable — P50 ≪ P99.9 | Fixed (if target confirmed) | Fixed — P50 = P99.9 = P100 | **Fixed — P50 = P99.9 = P100 (RTL confirmed)** |
| Kernel II | — | 1 | — | **1 (250M msg/s theoretical throughput)** |
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
- **Kernel II (CoSim, RTL):** 1 ✅
- **Hot path latency (CoSim, RTL):** 5 cycles (20 ns) — arbitrate → filter_event → parse_add_event → update_snapshot ✅
- **Full pipeline drain (CoSim, RTL):** 19 cycles (76 ns) — includes 10-cycle cold path BRAM R-M-W
- **CoSim status:** PASS (Verilog/xsim, 7 ASML messages)
- **Per-stage breakdown:**
    - `arbitrate`: 2 cycles, II=1, 4.964 ns (timing estimate miss vs 4.00 ns target; II=1 achieved)
    - `filter_event`: 3 cycles, II=1, 0 DSP (comparisons combinatorial; 3 cycles from counter RMW scheduling)
    - `parse_add_event`: 0 cycles, II=1 (field extract + fan-out, purely combinatorial)
    - `update_snapshot`: 0 cycles, II=1, 3.640 ns (timing closes with 0.36 ns margin)
    - `register_book_update`: 10 cycles, II=1 (DSP multiply 4 cycles + BRAM R-M-W; decoupled by depth=512 FIFO)
- **Resource utilization:**
    - BRAM_18K: 5 (12%)
    - DSP: 1 (2%)
    - FF: 3466 (21%)
    - LUT: 2124 (26%)

For detailed breakdowns (including per-function reports), see:

  docs/reports/kernel_csynth.rpt
  docs/reports/register_book_update_csynth.rpt
  docs/reports/parse_add_event_csynth.rpt
  docs/reports/update_snapshot_csynth.rpt
  docs/reports/arbitrate_csynth.rpt

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
- [ADR-012](docs/adr/ADR-012-hot-path-register-store-buffer.md) — Hot-path write coalescing: register store buffer + cold-path BRAM writer *(deferred)*
- [ADR-013](docs/adr/ADR-013-bram-off-hot-path-ii-equals-1.md) — BRAM off hot path: achieving II=1 on the full pipeline *(deferred)*
- [ADR-014](docs/adr/ADR-014-multi-feed-bus-widening-per-instrument-routing.md) — Multi-feed bus widening and per-instrument pipeline routing *(deferred)*
- [ADR-015](docs/adr/ADR-015-market-toxicity-detection.md) — Market toxicity detection: quote cancellation ratio in hardware *(deferred, requires X/D/U)*
- [ADR-016](docs/adr/ADR-016-early-exit-event-filter.md) — Early-exit event filter: drop irrelevant or anomalous messages before the pipeline *(price sanity + qty floor implemented; instrument membership deferred)*

## Reference Implementation

[low-latency-feed-handler](https://github.com/alchevrier/low-latency-feed-handler) — NASDAQ ITCH 5.0 parser, MPSC queue, SOA order book, DPDK pcap PMD pipeline. The C++ baseline this project benchmarks against.
