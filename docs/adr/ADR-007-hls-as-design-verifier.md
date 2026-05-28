# ADR-007 — HLS as Design Verifier

## Status
Accepted

## Date
2026-05-28

## Context

The design goal is a hardware datapath with specific latency properties: 6-cycle parse-to-book-insert, II=1, at 250 MHz. There are multiple ways to implement and verify such a design:

| Approach | Verification | Cycle accuracy | Development cost |
|---|---|---|---|
| SystemVerilog / VHDL from scratch | RTL simulation | Exact | Very high |
| HLS (Vitis HLS / Catapult) | C sim + RTL cosim + synthesis report | Exact post-synthesis | Low to medium |
| Soft model (C++ only, no HLS) | Unit tests | None | Very low |

The design involves non-trivial micro-architectural choices: BRAM port allocation, DATAFLOW pipeline scheduling, II constraints, and register vs BRAM resource binding. These cannot be verified by a C++ model alone — the model says nothing about whether the chosen structure synthesises to the intended hardware.

SystemVerilog gives exact cycle-accurate simulation but requires writing and debugging RTL directly, which is significantly more time-intensive for exploring and iterating on the architecture.

HLS occupies a productive middle ground:
- The algorithm is written in C++ with pragmas, which is the same language as the reference implementation and is directly readable.
- Vitis HLS performs **C simulation** (functional correctness against a testbench), **RTL co-simulation** (cycle-accurate verification of the synthesised RTL driven by the C testbench), and produces a **synthesis report** giving exact cycle counts, II, resource utilisation, and achievable frequency.
- If the synthesis report shows II > 1 or latency > 6 cycles, the pragma configuration must be revised — HLS surfaces the violation, not a late-stage place-and-route failure.
- HLS-generated RTL can be handed off to a hardware engineer or converted to optimised SystemVerilog for the critical paths if needed. The HLS phase de-risks the architecture before that investment.

## Decision

Vitis HLS is used as the **primary design and verification tool** for this project. The development flow is:

1. **C simulation**: the kernel is compiled and run as ordinary C++ against a testbench feeding real ITCH messages. Functional correctness is established without any synthesis step.
2. **HLS synthesis**: pragmas drive the synthesis. The report confirms (or refutes) II=1, latency=6, and the 250 MHz timing target.
3. **RTL co-simulation**: the synthesised RTL is re-driven by the C testbench via a generated wrapper. Cycle-accurate verification without writing a separate SystemVerilog testbench.
4. **SystemVerilog for critical paths** (future): if HLS-generated RTL for `handle_event` does not close timing at 250 MHz, that stage is hand-written in SystemVerilog. The HLS phase identifies whether this is needed before committing the effort.

HLS is not used here because it is "easier" — it is used because it is the correct tool for **architecture exploration under cycle and resource constraints**, with an integrated verification flow that surfaces violations early.

## What HLS Verifies That C++ Cannot

- **II=1**: whether `handle_event` truly pipelines at one new input per cycle given the BRAM read-write pattern.
- **Resource binding**: whether `Level bids[]` is inferred as BRAM (not LUTRAM or registers) as intended.
- **Timing closure**: whether the combinatorial path for the mathematical index fits within a 4 ns clock period at 250 MHz.
- **Port conflicts**: whether BRAM port allocation across `handle_event` and any concurrent access violates dual-port constraints.

None of these questions can be answered by C++ compilation or unit tests.

## Consequences

- The primary deliverable of this phase is the **HLS synthesis report**: II, latency, and resource/timing numbers. Until synthesis is run, the 6-cycle figure is a design target derived from the stage decomposition in ADR-003.
- The benchmark comparison against the C++ baseline (274 ns P99.9) uses the projected cycle count: 6 cycles × 4 ns/cycle at 250 MHz = **24 ns** (to be confirmed by the synthesis report).
- If a future hardware engineer requires RTL, the HLS-generated RTL is the starting point. Hand-optimised SystemVerilog for `handle_event` is an identified future path if timing does not close. SystemVerilog is to HLS what assembly is to C: you only go there when the compiler does not believe you, and you need to express the exact register-transfer behaviour yourself.
