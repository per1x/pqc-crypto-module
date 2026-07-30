**English** · [中文](README.zh-CN.md)

# XC7Z020 (Zynq-7000) board support

This directory belongs to the `board/xc7z020` branch and is **not merged back into
`main`**. `main` stays vendor-neutral: no RTL there instantiates a vendor
primitive and no software there assumes a particular part. Everything
board-specific — the Vivado project, pin constraints, capacity trade-offs, real
MMIO addresses, image integration — lives here.

The vendor-neutral RTL is **referenced from its path in the main tree**, not
copied. A copy drifts, and it also makes "is the RTL cocotb verifies the RTL that
got synthesised?" unanswerable.

## Layout

```
board/xc7z020/
├── rtl/            board wrapper pqc_accel_zynq (AXI port naming + status LEDs)
├── vivado/         Tcl to create the project and block design, and to build a bitstream
├── constraints/    timing.xdc is board-independent; boards/<name>.xdc holds pins only
├── src/            real MMIO backend accel_zynq.c (UIO / /dev/mem + AXI-DMA)
├── include/        address map and AXI-DMA register offsets
├── tests/          MMIO backend timing verification (memory-backed maps, a thread plays hardware)
├── tools/          offline checks, resource budget, cross build
├── pynq/           PYNQ overlay and on-board self-test
├── petalinux/      device-tree fragment and build configuration
└── docs/           resource budget, Vivado flow, cross build
```

## System structure

```
        Cortex-A9 (armv7)                        PL
 ┌───────────────────────────┐   ┌────────────────────────────────┐
 │ libpqchsm                 │   │                                │
 │   pqc_backend_accel       │   │  pqc_accel_zynq                │
 │     accel_transport_zynq ─┼───┼─► AXI4-Lite  0x43C0_0000       │
 │       control: registers  │   │     control/status registers   │
 │       data:    AXI-DMA ───┼───┼─► AXI4-Stream ─► keccak_f1600  │
 └───────────────────────────┘   └────────────────────────────────┘
     DDR reserved 0x1F00_0000        axi_dma  0x4040_0000
```

The command sequence matches [docs/register-map.md](../../docs/register-map.md)
exactly, and matches the simulated backend exactly: set MODE/IN_LEN, move the
input into the PL, write CTRL.START, poll STATUS.DONE, move the result back.
Switching between simulation and hardware changes only how transactions are
issued.

## What the delivered configuration contains

The Keccak-f[1600] core only. The ML-KEM NTT core does not fit in an XC7Z020: it
measures 58008 LUT against a capacity of 53200. The full argument, what excluding
it costs, and what it would take to include it are in
[docs/resource-budget.md](docs/resource-budget.md).

Root of trust, PUF, secure boot, and eFUSE are **out of scope for this stage**.
Choosing XC7Z020 means the key derivation root stays a software constant and
device binding does not hold. That is already recorded as a validation gap in
§13 of [docs/security-policy.md](../../docs/security-policy.md); this branch does
not change it.

## Verified without a board (all passing)

```bash
board/xc7z020/tools/board_checks.sh          # runs the four checks below
board/xc7z020/tools/board_checks.sh --fast   # skips the resource budget (minutes of Yosys)
```

| Check | Criterion | Result |
|---|---|---|
| Board RTL static checks | Verilator `-Wall` clean + Yosys synthesises | pass |
| Vivado Tcl offline check | balanced braces + stubbed run reaches the last line | both scripts pass |
| cocotb, delivered configuration | with `INCLUDE_NTT=0`, codes 7/8 return ERRCODE=3 and the Keccak datapath is correct | 9 pass, 2 skip |
| Resource budget | delivered configuration under 70% of the device | 34.4%, pass |

Two more run on the host:

```bash
ctest --test-dir build -R accel_zynq          # MMIO backend register and DMA sequencing
board/xc7z020/tools/armv7_test.sh             # armv7 cross build plus the suite under QEMU
```

The `accel_zynq` case replaces the mappings with memory and runs a thread that
plays hardware according to the AXI-DMA and register contracts. It found a real
defect: the driver reported success when the DMA had not delivered the result.

## Requires a board

Each item below has a script or a document ready, but **none has ever been
executed**: the development machine has no Vivado, no PetaLinux, and no board.

| Step | How | Needs |
|---|---|---|
| Create the Vivado project | `vivado -mode batch -source vivado/create_project.tcl` | Vivado |
| Build bitstream and .hwh/.xsa | `vivado -mode batch -source vivado/build_bitstream.tcl -tclargs -proj <path>.xpr` | Vivado |
| Check resource usage | compare `outputs/utilization.rpt` against docs/resource-budget | Vivado |
| Confirm timing closes | WNS/WHS positive in `outputs/timing.rpt` (the script already fails on negative) | Vivado |
| Confirm PS7 DDR/MIO | install board files for the official preset, or the image will not boot | board files |
| Verify LED pins | check against the official master XDC for the board in hand | board docs |
| Build a bootable image | see [petalinux/README.md](petalinux/README.md) | PetaLinux |
| On-board self-test | see [pynq/README.md](pynq/README.md) | board |
| Confirm the address map | reading VERSION must give 0x0001_0000 | board |
| Measure end-to-end speedup | run `pqchsm-prim-bench` on the board | board |

Triage order for first power-on: LED0 dark → reset not released or clock not
configured; VERSION does not read 0x0001_0000 → address map disagrees with the
table; VERSION correct but DONE never sets → data path or clock domain; DONE sets
but the result is wrong → suspect the cache attributes of the DMA buffer first.

## Moving to another XC7Z020 board

Two things only:

1. Add a `<name>.xdc` following the shape of `constraints/boards/pynq_z2.xdc`,
   filling in LED pins and I/O standards. `constraints/timing.xdc`, the RTL, the
   block design, and the software all stay untouched.
   `constraints/boards/ax7020.xdc` is a prepared template with the pins left
   blank.
2. Generate the project with `-board <name>` and confirm the PS7 DDR/MIO preset
   against that board's documentation.

No guessed pin numbers appear in this directory: LED pins have no commonality
across XC7Z020 boards from different vendors, and a wrong one connects an output
to another output.
