**English** · [中文](README.zh-CN.md)

# Synthesis

Out-of-context synthesis scripts producing utilisation and timing reports.

> **Status: in use.** Vivado is not installed on the Mac, so this runs on the build
> machine (Vivado 2020.1). Every utilisation and Fmax figure in `docs/TESTING.md`
> comes from it, taken **post-route** — post-synthesis WNS is usually optimistic.

```bash
vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs <part> <top_module>
# e.g.
vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs xazu3eg-sfvc784-1-i mlkem_encaps
```

**Both tclargs are required, part first and top second.** Leaving out the part makes the
script print its usage and exit — easy to lose a run to.

Synthesisable top levels: `mont_reduce`, `barrett_reduce`, `butterfly_ct`,
`butterfly_gs` (combinational, constrained by `ooc_comb.xdc`); `ntt_core` and
`keccak_f1600` (clocked, constrained by `ooc_seq.xdc`). The script selects the
constraint file automatically.

`keccak_f1600` is the most informative one to run first: its utilisation directly tests
the decision to build a single-round iterative core rather than a 24-round unrolled one.
1600 state flip-flops plus one round of combinational logic should land in the low
thousands of LUTs; unrolled, the logic is multiplied by 24 and will not fit on a small
device. A synthesis report turns that from a design judgement into a number.

Common parts:

| Board | Part |
|---|---|
| Kria KV260 (UltraScale+) | `xck26-sfvc784-2LV-c` |
| PYNQ-Z2 / Arty Z7-20 | `xc7z020clg400-1` |
| Arty A7-100T | `xc7a100tcsg324-1` |

## Why run this before buying a board

Utilisation, timing closure, and Fmax estimation do not need hardware — only a target
part. Even `write_bitstream` runs; there is simply nowhere to load it. The one figure
that remains an estimate is power, from a toggle-rate model with error up to 2×.

The consequence is practical: choosing a board without a synthesis report is guesswork.
If an 8-butterfly NTT core does not fit on an XC7A100T, that is worth knowing before
ordering an Arty A7.

## What can be decided without Vivado

`tools/cycle_budget.py` computes the NTT cycle budget and the parallelism trade-off from
first principles:

```
$ python3 tools/cycle_budget.py --fmax 150
ML-KEM 256-point NTT: 128 butterflies per layer x 7 layers = 896 butterfly operations
  1 butterfly   ->  910 cycles -> 6.07 us
  4 butterflies ->  238 cycles -> 1.59 us   (needs 2-4 BRAM banks)
  8 butterflies ->  126 cycles -> 0.84 us   (memory ports become the bottleneck, 4-8 banks)
```

The intended order is: fix the micro-architecture from this table, then write RTL, then
use Vivado to check that the calculated numbers and the synthesised ones agree.

## Layout

```
hardware/syn/
├── ooc_synth.tcl            OOC synthesis + place & route, prints Fmax
├── constraints/ooc_seq.xdc  Clocked modules: real clk port, 100 MHz, I/O delay, rst_n false path
├── constraints/ooc_comb.xdc Combinational modules: virtual clock, in-to-out only
└── rpt/                     Report output (gitignored)
```

## Why the constraints are split

A clocked module must be constrained through its real clock port. Under a virtual clock
bound to no port the `clk` port is unconstrained, no timing path is analysed, and
`report_timing_summary` produces a distorted — and misleadingly favourable — result.
Combinational modules, having no clock port, need the opposite treatment.

`ooc_synth.tcl` routes each module to the appropriate constraint file:

| Module | Constraints | Notes |
|---|---|---|
| `ntt_core`, `keccak_f1600`, `sha3_core`, `pqc_accel_axi` | `ooc_seq.xdc` | `create_clock -period 10 [get_ports clk]`, I/O delay, `rst_n` false path |
| `mont_reduce`, `butterfly_ct`, `butterfly_gs` | `ooc_comb.xdc` | virtual clock, combinational path delay |

**Why 100 MHz rather than 150 MHz.** The butterfly contains two serial multiplications
(`zeta·b`, and `m·Q` inside the Montgomery reduction) plus a modular reduction. 6.667 ns
is tight; a credible positive WNS at 100 MHz is more useful than an optimistic failure
at 150.

Measured after S3 (coefficients moved into a true dual-port BRAM): `ntt_core` alone
closes with WNS `+0.077 ns`; the whole `pqc_accel_axi` top misses by `-0.047 ns`
(2 endpoints out of 6190, Fmax 99.5 MHz). The critical path is that same butterfly
multiplication chain, now fed by the BRAM output — of its 9.693 ns, 0.998 ns is the
BRAM clock-to-out. Two ways to close it: enable the BRAM optional output register
(`DO_REG=1`, which Vivado suggests in the synthesis log) at the cost of one more cycle
of read latency everywhere, or pipeline the butterfly itself — a register after the
Montgomery output — at the cost of one more cycle per butterfly. Neither is done yet:
99.5 MHz makes no difference to "does it fit", and S4 has to re-time this area anyway.
