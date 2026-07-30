**English** · [中文](resource-budget.zh-CN.md)

# XC7Z020 resource budget

## Conclusion first

The configuration delivered for XC7Z020 **excludes the ML-KEM NTT core**. It
carries the Keccak-f[1600] core and the bus interface only. The reason is
measured, not estimated:

| Configuration | LUT | Share of 53200 | Verdict |
|---|---|---|---|
| Keccak + bus (delivered) | 18307 | 34.4% | fits, with room to spare |
| Keccak + NTT + bus | 58350 | 109.7% | **does not fit** |

The numbers come from `board/xc7z020/tools/resource_budget.sh` and can be
regenerated. That script also enforces the budget: it exits non-zero if the
delivered configuration exceeds 70% of the device. A budget that cannot fail is
prose, not a check.

## Device capacity

XC7Z020 — the part on PYNQ-Z2, Alinx AX7020, and similar boards:

| Resource | Count |
|---|---|
| LUT | 53200 |
| Flip-flops | 106400 |
| DSP48E1 | 220 |
| 36Kb BRAM | 140 |
| URAM | none |

## Measurements

Yosys synthesised to 7-series cells (`synth_xilinx -family xc7 -flatten`):

| Module | LUT | FF | DSP | BRAM36 | MUXF7/8 |
|---|---|---|---|---|---|
| pqc_accel_zynq (delivered) | 18307 | 6071 | 0 | 0 | 844 |
| pqc_accel_zynq (with NTT) | 58350 | 10271 | 13 | 0 | 23184 |
| ntt_core (ML-KEM, 7 layers) | 23228 | 4146 | 13 | 0 | 13182 |
| keccak_f1600 | 5864 | 1607 | 0 | 0 | 149 |
| mldsa_ntt_core (8 layers) | 45659 | 8242 | 31 | 0 | 26584 |
| mlkem_rej_uniform | 6250 | 3083 | 0 | 0 | 2545 |
| mldsa_rej_uniform_buf | 2713 | 5899 | 0 | 0 | 830 |
| trng_health | 136 | 53 | 0 | 0 | 2 |
| axi4lite_regs | 392 | 243 | 0 | 0 | 7 |

AXI-DMA and the interconnect are Xilinx IP and are not in the table: `axi_dma`
(simple mode, 32-bit) is roughly 1200 LUT / 1600 FF, and an AXI interconnect
costs roughly 400 LUT per slave port.

## What these numbers are, and are not

**They are not Vivado implementation results.** Yosys uses an open 7-series cell
library and packs differently (LUTs into slices, FF/LUT sharing, SRL inference,
BRAM and distributed-RAM inference policy). Vivado usually reports fewer LUTs.
Before going to hardware, generate `utilization.rpt` with
`board/xc7z020/vivado/build_bitstream.tcl` and treat that as authoritative.

**The conclusion does not depend on the gap.** The delivered configuration uses
34%; it would still fit if Vivado came in twice as high. The configuration with
the NTT exceeds 110%; it would still not fit if Vivado saved a third. There is
margin in both directions.

## Why the NTT core is expensive

`ntt_core` holds 256 coefficients of 16 bits and **writes two addresses per
cycle**: one butterfly produces two outputs, written back to `mem[j]` and
`mem[j+len]`. No FPGA RAM primitive supports two writes per cycle — block RAM
and distributed RAM each have a single write port. The array therefore becomes
flip-flops, plus

- three read-port multiplexers (`a_val`, `b_val`, `scale_in`), each 256-to-1 and
  16 bits wide;
- two write-address decoders.

The 13182 MUXF7/MUXF8 cells Yosys reports are exactly those multiplexer trees.
This is not weak inference on Yosys's part: Vivado has the same options for the
same RTL. **The constraint comes from the number of write ports, not from the
tool.**

Putting the core back requires reorganising the coefficient storage into banks
so that each bank is written at most once per cycle, at which point it maps to
real RAM. The comment in `hardware/rtl/mlkem/ntt_core.v` already notes that
raising the butterfly count requires splitting coefficients across banks; that
same change resolves the area problem. The ML-DSA NTT core (45659 LUT) is more
expensive still and follows the same path.

## What excluding the NTT costs

Per `tools/amdahl.py` in the main branch, SHAKE accounts for roughly 55% of
ML-KEM-768 and NTT-related work for roughly 30%. Accelerating Keccak alone
bounds the end-to-end speedup at about 2.09×; accelerating the NTT alone, about
1.37×. Given room for only one, Keccak was already the first choice — excluding
the NTT gives up the smaller share.

With the core excluded, operation codes 7 and 8 return `ERRCODE=3` exactly as any
other unimplemented mode does; no second way of saying "unsupported" is
introduced. `board/xc7z020/tools/cocotb_ship_config.sh` runs the cocotb suite
with `INCLUDE_NTT=0` to confirm that behaviour holds and that the Keccak datapath
still works.

To include the NTT again:

```bash
vivado -mode batch -source board/xc7z020/vivado/create_project.tcl -tclargs -with-ntt
```

The flag is passed through to `INCLUDE_NTT` on `pqc_accel_axi`. On XC7Z020 this
overflows the device; it is meaningful only on a larger part such as XC7Z035 or
XC7Z045.

## Clock and timing

The PL clock is PS7 `FCLK_CLK0`, set to **100 MHz** in `create_project.tcl`.

**There is no evidence that timing closes.** Vivado is not installed on the
development machine and implementation has never been run. The longest path in
the design is the single Keccak round (θ→ρ→π→χ→ι); whether it holds at 100 MHz is
a question only the implementation report can answer. `build_bitstream.tcl` reads
WNS/WHS and exits non-zero when either is negative — timing that does not close
is not a warning, it makes the accelerator produce wrong results unpredictably.

If it does not close there are two options: lower
`PCW_FPGA0_PERIPHERAL_FREQMHZ`, or add a pipeline stage inside the Keccak round
(each permutation then takes 48 cycles instead of 24, which is still ample for
the rate at which the PS calls it).

## Three things to check on hardware

1. Whether `utilization.rpt` is in the same order of magnitude as the estimates
   here. A large discrepancy means inference behaved differently than expected,
   and the memory in question needs revisiting.
2. Whether WNS/WHS in `timing.rpt` are positive.
3. Whether `.bit` and `.hwh` are the same name and same version. PYNQ pairs them
   by filename; a mismatch gives the Overlay a stale address map, which presents
   as registers that read back values with no meaning.
