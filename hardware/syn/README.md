**English** · [中文](README.zh-CN.md)

# Synthesis

Vivado scripts for module-level out-of-context (OOC) synthesis and for the full board
bitstream. Vivado is not installed on the Mac; the scripts run on the build machine
(Vivado 2020.1). Fmax figures are derived from post-route WNS, because post-synthesis
WNS is usually optimistic. Reports are written to `hardware/syn/rpt/` (gitignored) and
bitstream outputs to `hardware/syn/impl/`.

## Files

| File | Purpose |
|---|---|
| `ooc_synth.tcl` | OOC synthesis, place and route of one module; prints Fmax and writes `rpt/<top>_fmax.txt` |
| `ooc_mldsa.tcl` | The same for `mldsa_keygen`, `mldsa_sign`, `mldsa_verify` or `mldsa_engine`; reports are named by parameter set |
| `trng_ooc.tcl` | OOC synthesis, place and route of the TRNG (default top `trng_axi`) |
| `impl_bitstream.tcl` | Non-project flow for `zu3eg_hsm_top`, producing `.bit` and `.bin` in `syn/impl/` |
| `constraints/ooc_seq.xdc` | Clocked modules: `clk` port at 100 MHz, 1 ns input and output delay, `rst_n` false path |
| `constraints/ooc_comb.xdc` | Combinational modules: virtual clock, input-to-output paths only |
| `constraints/trng_ro.xdc` | TRNG: clock, I/O budget, `rst_n` and `tamper` false paths |
| `constraints/board_pins.xdc` | AXU3EGB pin constraints; the only pin is the fan PWM on `AA11` |

## ooc_synth.tcl

```bash
vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs <part> <top_module>
# e.g.
vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs xazu3eg-sfvc784-1-i mlkem_encaps
```

Both arguments are required, part first. With either missing the script prints its usage
and exits.

The script reads `rtl/common`, `mlkem`, `mldsa`, `keccak`, `bus`, `sym` and `trng`. The
tops below get `ooc_seq.xdc`; any other name gets `ooc_comb.xdc`, so a new clocked top
must be added to the list in `ooc_synth.tcl` first.

| Constraint file | Tops |
|---|---|
| `ooc_seq.xdc` | `ntt_core`, `mldsa_ntt_core`, `keccak_f1600`, `sha3_core`, `pqc_accel_axi`, `mlkem_keygen`, `mlkem_encaps`, `mlkem_decaps`, `mlkem_cbd_stream`, `mlkem_bitpack`, `mlkem_bitunpack`, `key_vault`, `key_vault_axi`, `axi4lite_firewall`, `aes_core`, `sm4_core`, `sm3_core`, `sym_axi`, `sym_vault_top` |
| `ooc_comb.xdc` | everything else, intended for `mont_reduce`, `barrett_reduce`, `butterfly_ct`, `butterfly_gs` |

A clocked module has to be constrained through its real clock port. With a virtual clock
bound to no port, `clk` is unconstrained, no path is timed, and the timing summary looks
better than it is.

The OOC clock is 100 MHz (10 ns) rather than 150 MHz because the ML-KEM butterfly has two
serial multiplications (`zeta·b`, and `m·Q` inside the Montgomery reduction) plus a
modular reduction. The board runs the crypto cores at 75 MHz. `ntt_core` splits the
butterfly with a register between `_head` and `_tail`, which is why a transform takes
3457 cycles.

## ooc_mldsa.tcl

```bash
vivado -mode batch -source hardware/syn/ooc_mldsa.tcl \
       -tclargs <part> <keygen|sign|verify|engine> <44|65|87>
# e.g.
vivado -mode batch -source hardware/syn/ooc_mldsa.tcl -tclargs xazu3eg-sfvc784-1-i sign 87
```

The ML-DSA cores select the parameter set at run time, so one synthesis covers 44, 65
and 87. The last argument only names the reports (`mldsa_<op>_<pset>_{utilization,timing}_{synth,route}.rpt`).

## trng_ooc.tcl

```bash
vivado -mode batch -source hardware/syn/trng_ooc.tcl
vivado -mode batch -source hardware/syn/trng_ooc.tcl -tclargs <part> <top>
```

It reads `trng`, `common` and `keccak`. Combinational loops are allowed on the ring
oscillator nets and DRC `LUTLP-1` is lowered to a warning. After synthesis and again
after routing it counts the LUTs under `u_ro*` and fails if fewer than 120 remain
(8 rings of 13 to 27 stages give 160), since a ring that was optimised away leaves a
constant generator and no error in the log. Do not define `TRNG_SIM_MODEL`; it replaces
`ring_osc` with a simulation model that cannot be synthesised.

## impl_bitstream.tcl

```bash
vivado -mode batch -source hardware/syn/impl_bitstream.tcl                 # zu3eg_hsm.bit
PQC_DEV_OPEN=1    vivado -mode batch -source hardware/syn/impl_bitstream.tcl   # zu3eg_hsm_dev.bit
PQC_CHARACTERIZE=1 vivado -mode batch -source hardware/syn/impl_bitstream.tcl  # zu3eg_hsm_char.bit
```

The flow creates the PS (`zynq_ultra_ps_e`, `M_AXI_HPM0_LPD` and `pl_clk0` at 150 MHz),
reads `rtl/*` and `platform/fan_ctrl`, and runs synthesis, `opt_design`, `place_design`,
`phys_opt_design`, `route_design`, a hold-fix pass, and `write_bitstream`. The PS
configuration here only defines the PL-side ports; the PS setup on the board comes from
the vendor `psu_init` in `BOOT.BIN`.

| Variant | Environment | Result |
|---|---|---|
| default | none | `zu3eg_hsm.bit`: functional slaves with `SECURE_ONLY=1` |
| development | `PQC_DEV_OPEN=1` | `zu3eg_hsm_dev.bit`: functional slaves with `SECURE_ONLY=0`; the key-register load port and seed ports stay secure-only |
| characterisation | `PQC_CHARACTERIZE=1` | `zu3eg_hsm_char.bit`: lowered fan thresholds and the raw TRNG tap open; not for product use |

The script stops before `write_bitstream` when any of these fail:

- The `tamper_latched` flip-flops are missing from the netlist (the `tamper` path was
  optimised to a constant).
- The `fan` port is not on `AA11`, or `SYSMONE4` is missing or its `SIM_DEVICE` is not
  `ZYNQ_ULTRASCALE`.
- A PS clock or response input (`maxihpm0_lpd_aclk`, `maxigp2_rlast`, `maxigp2_bid`,
  `maxigp2_rid`) has no driver. Vivado does not report a wire used before its
  declaration; it creates an undriven net.
- Setup WNS or hold WHS is negative, or the effective hold margin is below 0.050 ns.
  The script adds 0.100 ns of hold uncertainty on the system clock so that the router
  inserts delay on short paths, and the reported WHS already includes it.

Timing is written to `syn/impl/timing_<name>.txt`. The `.bin` is the raw bitstream that
`fpga_manager` and `fpgautil` load.

## Result for the default bitstream

Post-route, `zu3eg_hsm.bit` at 75 MHz on `xazu3eg-sfvc784-1-i` (build machine report,
2026-10-10):

| Resource | Used |
|---|---|
| LUT | 55,366 (78.5 %) |
| Registers | 39,990 |
| Block RAM tiles | 85 |
| DSP | 199 |
| Setup WNS | +1.615 ns |

## Parts

| Board | Part |
|---|---|
| AXU3EGB (the board used here) | `xazu3eg-sfvc784-1-i` |
| Kria KV260 | `xck26-sfvc784-2LV-c` |
| PYNQ-Z2 / Arty Z7-20 | `xc7z020clg400-1` |
| Arty A7-100T | `xc7a100tcsg324-1` |

`xazu3eg-sfvc784-1-i` is the automotive XA device on the AXU3EGB, with the XCZU3EG
resources: 70,560 LUT, 141,120 FF, 360 DSP, 216 BRAM36.

## Planning before RTL

`tools/cycle_budget.py` computes the ML-KEM NTT cycle budget for 1 to 16 parallel
butterflies, with a rough DSP and bank estimate:

```
$ python3 tools/cycle_budget.py --fmax 150
```

It only covers the NTT. `tools/amdahl.py` estimates the end-to-end speedup of different hardware splits (Keccak, NTT) from profiling fractions.
