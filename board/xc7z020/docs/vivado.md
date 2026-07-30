**English** · [中文](vivado.zh-CN.md)

# Vivado flow

**Vivado is not installed on this repository's development machine and the flow
below has never been executed.** The scripts are complete and pass the offline
checks (balanced braces; every Vivado command replaced with a stub and the script
run to completion), but nothing supports the claims that synthesis passes, timing
closes, or the bitstream works. Read every report the first time through.

## Why the project is not checked in

Vivado's `.xpr` and `.bd` carry absolute paths and tool versions. One setting
change produces a large unreviewable diff, and they frequently fail to open on a
different machine. What is checked in is the Tcl that generates them: the project
can be deleted and rebuilt at any time, and the artefact under review is the
script.

## Creating the project

```bash
vivado -mode batch -source board/xc7z020/vivado/create_project.tcl
```

Arguments:

| Argument | Default | Meaning |
|---|---|---|
| `-board <name>` | `pynq_z2` | selects `constraints/boards/<name>.xdc` and the PS7 preset |
| `-part <part>` | `xc7z020clg400-1` | device |
| `-outdir <path>` | `<repo>/build-vivado` | project output directory |
| `-with-ntt` | off | include the ML-KEM NTT core. Overflows XC7Z020; see the resource budget |

The block design produced:

```
PS7 ── M_AXI_GP0 ──┬── pqc_accel_zynq  S_AXI      0x43C0_0000
                   └── axi_dma_0       S_AXI_LITE 0x4040_0000
PS7 ── S_AXI_HP0 ──── both memory-mapped masters of axi_dma_0
axi_dma_0 M_AXIS_MM2S ──► pqc_accel_zynq s_axis
axi_dma_0 S_AXIS_S2MM ◄── pqc_accel_zynq m_axis
FCLK_CLK0 = 100 MHz, reset through proc_sys_reset
status_led[2:0] brought to the top level, pinned by the board XDC
```

The addresses must match the table in
`board/xc7z020/include/pqc_accel_zynq.h`. Changing one means changing the other:
with a wrong address the software still reads values, they just mean nothing,
which is the hardest class of fault to diagnose on hardware.

## Board files and DDR

**PS7 DDR timing, MIO assignment, and clock source frequency are board
parameters.** They differ entirely between boards and cannot be invented in the
script. Two cases:

**Board files installed** (recommended). When the script finds the matching board
part it applies the official preset and DDR and MIO are correct. Install by
placing the vendor's board files under

```
<Vivado install>/data/xhub/boards/XilinxBoardStore/boards/
```

or point `BOARD_REPO_PATHS` at the directory holding them; after restarting
Vivado, `get_board_parts` lists them. PYNQ-Z2 board files come from TUL, AX7020
from ALINX.

**No board files.** The script says so explicitly and configures only what
concerns the PL (GP0 master, HP0 slave, FCLK0 frequency). DDR keeps Vivado's
defaults — the project will synthesise, implement, and produce resource and
timing reports, but **it cannot boot a real board**, because the DDR parameters
are wrong.

## Building the bitstream

```bash
vivado -mode batch -source board/xc7z020/vivado/build_bitstream.tcl \
       -tclargs -proj <outdir>/pqc_accel_pynq_z2/pqc_accel_pynq_z2.xpr -jobs 8
```

Artefacts land in `outputs/` under the project directory:

| File | Purpose |
|---|---|
| `*_wrapper.bit` | bitstream |
| `pqc_accel_bd.hwh` | hardware handoff, required by PYNQ's `Overlay` |
| `*_wrapper.xsa` | hardware platform description, required by PetaLinux |
| `utilization.rpt` | resource usage, to compare against the resource budget |
| `timing.rpt` | timing summary |

The script fails and exits in two places rather than treating a problem as a
warning:

- synthesis or implementation did not reach 100%;
- WNS or WHS is negative. Timing that does not close makes the accelerator
  produce wrong results unpredictably, and carrying negative slack forward is
  pointless. If it does not close, lower
  `PCW_FPGA0_PERIPHERAL_FREQMHZ`, or add a pipeline stage inside the Keccak
  round's combinational logic.

## Offline check

After editing the Tcl, run

```bash
board/xc7z020/tools/tcl_check.sh
```

It does two things: uses `info complete` to check that braces, brackets, and
quotes balance; then replaces every Vivado command with a stub and runs each
script to completion in a real `tclsh`. The second step genuinely walks every
branch, so a misspelled variable, an inverted condition, or a missing `incr`
surfaces immediately.

It cannot prove that the arguments to Vivado commands are right — only a machine
with Vivado can. It proves that the script itself runs to the end.
