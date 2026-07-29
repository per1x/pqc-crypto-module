**English** · [中文](register-map.zh-CN.md)

# Accelerator register map

This document is the contract between the software accelerator backend
(`src/hal/`, `include/pqchsm/accel.h`) and the hardware accelerator
(`hardware/rtl/bus/`). Both sides are written against it, and the cocotb
testbench `hardware/tb/cocotb/test_axi.py` verifies each clause separately.

The control plane is AXI4-Lite; the data plane is AXI4-Stream. AXI is an ARM
standard, not a vendor interface, so the RTL that implements it stays as portable
as the algorithm cores: plain inferrable Verilog, no vendor IP or interconnect
primitive instantiated anywhere.

## Address map

All registers are 32 bits and word-aligned. Address decoding uses bits `[4:2]`.

| Offset | Name | Software | Description |
|---|---|---|---|
| `0x00` | `CTRL` | write | `[0]` START, `[1]` SOFT_RESET |
| `0x04` | `STATUS` | read | `[0]` DONE, `[1]` BUSY, `[2]` ERR |
| `0x08` | `MODE` | read/write | Operation code |
| `0x0C` | `PARAM` | read/write | Parameter set |
| `0x10` | `IN_LEN` | read/write | Input length in bytes |
| `0x14` | `OUT_LEN` | read | Output length in bytes, written by hardware |
| `0x18` | `ERRCODE` | read | Failure detail, written by hardware |
| `0x1C` | `VERSION` | read | Constant, `0x0001_0000` |

## Behavioural contract

**START self-clears.** Writing `1` to `CTRL[0]` starts a command; the hardware
clears the bit in the same cycle and `CTRL` always reads back `0`. Without this,
any read-modify-write of `CTRL` by software would retrigger the command.

**SOFT_RESET self-clears** as well. It resets the datapath, the status bits, and
both stream pointers, and leaves `MODE`/`PARAM`/`IN_LEN` untouched.

**DONE is a latched level, not a pulse.** It is set when a command finishes and
stays set until the next START. Software polls `STATUS` at arbitrary times; a
one-cycle pulse would be missed. Clearing is tied to START rather than to a read
of `STATUS`, so reading the register can never destroy the state it reports.

**ERR accompanies DONE.** A failed command sets both, in the same cycle, and
leaves the failure detail in `ERRCODE`. There is no separate "failed" completion
path for software to poll.

**BUSY reflects the datapath**, not a latched bit: it is high from START until
completion.

**STATUS, OUT_LEN, ERRCODE are hardware-written and software read-only.** Writes
to them are ignored and answered `OKAY` rather than `SLVERR`: on AXI4-Lite a write
to a read-only register is a benign operation, and answering with an error would
strand masters that do not know the convention.

**Unmapped addresses** read as zero and ignore writes, always answering `OKAY`.
Because decoding only looks at `[4:2]`, addresses at and above `0x20` alias back
onto the same registers; the size of the address space assigned by the system
limits the reachable range.

## Operation codes

| Code | Operation | `IN_LEN` | `OUT_LEN` |
|---|---|---|---|
| 7 | NTT, forward | 512 | 512 |
| 8 | NTT, inverse | 512 | 512 |
| 9 | Keccak-f[1600] | 200 | 200 |

Any other code, or a length that does not match the code, completes with `ERR`
set and `ERRCODE = 3` ("mode not implemented"). Reporting the failure is
deliberate: silently falling back to a different implementation would make a
partially implemented accelerator look complete.

The remaining codes in `accel_mode_t` — full ML-KEM and ML-DSA operations — are
defined in the software header and implemented by the software backends, but have
no hardware implementation. They are the codes that return `ERRCODE = 3`.

## Data plane

The data plane is 32 bits wide. The buffer is addressed in words and byte order
is little-endian, matching the software side.

**Input.** Each AXI4-Stream packet is written starting at buffer offset 0. After
the beat carrying `TLAST` is accepted, the write pointer returns to 0, so the next
packet starts from the beginning again. Software therefore needs no separate
"reset the write pointer" register. `TREADY` is low while `BUSY` is set: the
accelerator does not accept new input during a command.

**Output.** Once a command completes, `OUT_LEN` converted to words becomes
readable. Raising `TREADY` drains the result, with `TLAST` on the final beat.
Draining resets the read pointer and drops `TVALID`, so a result can be taken
exactly once; taking it again requires reissuing the command.

## Command sequence

```
stream input packet (TLAST on the last beat)
write MODE
write IN_LEN
write CTRL = START
poll STATUS until DONE
if ERR:  read ERRCODE
else:    read OUT_LEN, then drain the output stream
```

This is the same sequence the software backends in `src/hal/` follow, which is why
the software stub, the Verilator-simulated RTL, and a future memory-mapped
transport are interchangeable below `pqc_backend_t`.

## Verification

`hardware/tb/cocotb/test_axi.py` covers the clauses above one by one — reset
values, the `VERSION` constant, read/write registers, byte strobes, START
self-clearing, read-only registers, DONE latching, unsupported operation codes,
both datapaths against the Python reference model, stream handshake gaps, `BUSY`
gating the input stream, and SOFT_RESET. The AXI bus functional model is written
by hand in the testbench and pulls in no third-party AXI library.
