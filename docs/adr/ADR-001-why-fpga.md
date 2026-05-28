# ADR-001 — Why FPGA

## Status
Accepted

## Date
2026-05-28

## Context

The C++ reference implementation (`low-latency-feed-handler`) achieves a P99.9 parse-to-book-insert latency of **274 ns** on a DRAM-backed order book across 8,669 symbols on a real NASDAQ ITCH 5.0 dataset. This is measured end-to-end: DPDK pcap PMD → ITCH parser → SOA order book.

That 274 ns figure represents the ceiling of what the application code alone can achieve: zero-allocation pipeline, lock-free MPSC queue, cache-line-aligned SOA arrays, dedicated isolated cores, suppressed OS tick. At the code level, the only remaining avenue is eliminating residual copies — a `memmove` when inserting a new price level into the SOA order book to shift existing levels and make room — and that is single-digit nanoseconds at best. Further reductions are possible through system-level tuning — BIOS power management, NUMA pinning, IRQ affinity, hugepage configuration, kernel build flags — but those close the gap by tens of nanoseconds at best. The remaining latency is not a code problem.

The primary source of remaining latency on the CPU path is non-determinism:

- **Cache misses**: DRAM access latency is data-dependent. A cold price level access costs ~70 ns; a hot L1 hit costs ~1 ns. P99.9 reflects the DRAM tier.
- **Branch misprediction**: binary search over price levels stalls the pipeline on each mispredicted branch.
- **OS jitter**: even with `isolcpus`, `nohz_full`, and `rcu_nocbs`, kernel interference is not eliminated — only reduced. Non-moveable IRQs fire on isolated cores regardless of affinity settings. RCU callbacks are offloaded by `rcu_nocbs` but not eliminated. Scheduler residuals remain even on a core with no runnable tasks.
- **MESI coherency traffic**: multi-core cache coherency adds non-deterministic latency when producer and consumer threads share cache lines.

An FPGA removes all of these sources:

- No cache hierarchy — BRAM access latency is **fixed at 2 cycles every time**, unconditionally.
- No branch predictor — there is no predictor to miss. Logic is combinatorial or pipelined by construction.
- No OS — the FPGA is a bare-metal datapath. There is no scheduler, no interrupt, no kernel path.
- No MESI — there is no coherency protocol. Each block RAM has fixed port access patterns determined at synthesis time.

## Decision

Use an FPGA (Xilinx/AMD UltraScale+, targeted via Vitis HLS) as the execution substrate for the NASDAQ ITCH 5.0 parse-to-book pipeline.

The design goal is a **6-cycle, deterministic parse-to-book-insert latency** at 250 MHz (= 24 ns).

All figures below are order-of-magnitude indicators. The C++ numbers are from a specific machine (Intel i5-12400) under specific benchmark conditions; production hardware and system configuration will shift them. The FPGA figure is derived from HLS synthesis — actual silicon latency depends on place-and-route and clock distribution.

This is not primarily a median-latency improvement. The C++ baseline achieves ~20 ns P50 on the real ITCH dataset — the top symbols stay L1-resident due to the market's power law, and the compute floor is roughly the same as the FPGA target. The FPGA wins by **eliminating the tail**: C++ P99.9 is 274 ns (illiquid symbols hitting DRAM); FPGA P99.9 is 24 ns — the same as P50, because there is no cache, no miss, and no variable-latency path. P50 = P99 = P99.9 = P99.99 = P100 = 6 cycles.

## Consequences

- The implementation language is C++ with Vitis HLS pragmas. The same language as the reference implementation; different compilation target.
- All data structures must be FPGA-resource-aware: BRAM for the order book, UltraFast registers for snapshot state.
- The design is single-symbol, single-instrument in this phase. Multi-symbol generalisation is a future ADR.
- Benchmarking methodology changes: cycle counts and synthesis reports replace `clock_gettime` wall-clock measurements.
