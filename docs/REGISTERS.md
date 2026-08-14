**English** · [中文](REGISTERS.zh-CN.md)

# Register reference

The contract between software and every AXI slave in the design. Both sides are
written against this document, and each clause is verified individually by a
cocotb testbench.

All registers are 32 bits and word-aligned. Every slave sits behind
`axi4lite_firewall`; addresses outside a slave's window, and non-secure accesses
to a `SECURE_ONLY=1` instance, are answered DECERR with no side effect.

> ### ⚠️ Read this before writing any register
>
> **The default bitstream (`zu3eg_hsm.bit`) gives the normal world *zero*
> reachability.** All four functional slaves are built with `SECURE_ONLY=1`, so
> every access from Linux — which always carries `AxPROT[1]=1` — is refused at
> the bus. Only the secure world (EL3, via the BL31 SiP) can drive them.
>
> **Never issue a write you expect to be refused.** A refused read returns
> DECERR synchronously and Linux turns it into `SIGBUS`, which a program can
> catch. A refused *write* is different: AXI writes are posted, so the error
> comes back later as an **SError**, which belongs to no instruction and which
> the kernel can only answer with a panic. **The cost is a power cycle**, and on
> this board a power cycle clears `CSU_MULTI_BOOT`.
>
> Programs that write registers (`hsm_hwtest`, `hsm_kem3`) must therefore run
> against the **development** bitstream, not the default one:
>
> ```
> PQC_DEV_OPEN=1 vivado -mode batch -source hardware/syn/impl_bitstream.tcl
> ```
>
> That build sets `SECURE_ONLY=0` on the functional slaves and is named
> `zu3eg_hsm_dev.bit` so the two can never be confused. It is a debug form, not
> the shipping one.

- [Slot map](#slot-map)
- [`trng_axi`](#trng_axi--slot-0)
- [`key_vault_axi`](#key_vault_axi--slot-1)
- [`sym_axi`](#sym_axi--slot-2)
- [`mlkem_axi`](#mlkem_axi--slot-3)
- [`pqc_accel_axi`](#pqc_accel_axi--simulation-and-host-path)
- [Shared conventions](#shared-conventions)

## Slot map

Base `0x8000_0000`, slot from `addr[18:16]`, 64 KB each. See
[ARCHITECTURE.md](ARCHITECTURE.md#address-map) for why the decode is exact.

| Slot | Address | Slave | `SECURE_ONLY` |
|---|---|---|---|
| 0 | `0x8000_0000` | `trng_axi` | 0 |
| 1 | `0x8001_0000` | `key_vault_axi` | 0 |
| 2 | `0x8002_0000` | `sym_axi` | 0 |
| 3 | `0x8003_0000` | `mlkem_axi` | 0 |
| 4 | `0x8004_0000` | canary (`key_vault_axi`) | **1** |
| 5 | `0x8005_0000` | `fan_ctrl_axi` | 0 |

## `trng_axi` — slot 0

Unlike the other slaves this is a **free-running, always-on** peripheral: there
is no command/complete cycle, only warm-up → ready → take words, plus health
status. RTL `hardware/rtl/trng/trng_axi.v`, tests
`hardware/tb/cocotb/test_trng_axi.py`.

| Offset | Name | Access | Description |
|---|---|---|---|
| `0x00` | `CTRL` | RW | `[0]` ENABLE (level), `[1]` ZEROIZE (write-1 pulse), `[2]` CLEAR_ALARM (write-1 pulse) |
| `0x04` | `STATUS` | R | See below |
| `0x08` | `RDATA` | R | **Each read pops one 32-bit random word** |
| `0x0C` | `HEALTH` | R | `[31:16]` current APT window count, `[15:0]` current RCT run |
| `0x10` | `APT_INDEX` | R | Samples processed in this APT window |
| `0x14` | `STARTUP` | R | Samples passed by the startup health test |
| `0x18` | `BLOCKS` | R | Rate blocks absorbed by the conditioner |
| `0x1C` | `WORDS` | R | Words delivered to software |
| `0x20` | `VERSION` | R | Constant `0x0001_0000` |
| `0x24` | `PARAM0` | R | `{DECIM, NUM_RO, RATE_LANES, OUT_LANES}`, one byte each |
| `0x28` | `PARAM1` | R | `{APT_CUTOFF[15:0], RCT_CUTOFF[15:0]}` |
| `0x2C` | `PARAM2` | R | `{STARTUP_SAMPLES[15:0], APT_WINDOW[15:0]}` |

`STATUS`: `[0]` READY (startup passed, no alarm, not wiping), `[1]` DATA_VALID,
`[2]` ALARM (latched), `[3]` RCT_ALARM, `[4]` APT_ALARM, `[5]` STARTUP_DONE,
`[6]` FIFO_WIPING, `[7]` ENABLED, `[8]` UNDERRUN (latched).

**`RDATA` pops on read.** One read consumes one word. Software must **not**
read-back-verify it, must not re-read the address to confirm, and must not dump
this range in a debugger — every read consumes entropy. AXI4-Lite has no
speculative reads, so a read is exactly one consumption.

**An empty read returns 0 and latches `UNDERRUN`.** Returning 0 is itself
dangerous: a driver that reads without checking status would use 0 as a random
number. `UNDERRUN` exists to catch that driver bug afterwards; it latches and
clears only via `ZEROIZE` or `CLEAR_ALARM`.

> Correct driver usage: read `STATUS` and confirm `DATA_VALID` **before** every
> `RDATA` read. After taking a batch, read `STATUS` again and confirm both
> `UNDERRUN` and `ALARM` are clear — otherwise discard the whole batch.

**`ALARM` is a latched level, not a pulse**, for the same reason as `DONE`
elsewhere: software polls at arbitrary times and would miss a single cycle.

The cutoffs in `PARAM1` are derived from the measured min-entropy, not assumed;
see [TESTING.md](TESTING.md#entropy).

## `key_vault_axi` — slot 1

Window `0x00`–`0x3F`; anything beyond is DECERR. RTL
`hardware/rtl/bus/key_vault_axi.v`, tests
`hardware/tb/cocotb/test_key_vault.py`.

This peripheral exists to make "keys do not leave the cryptographic boundary" a
**property of the circuit rather than a discipline**. The usual approach puts
keys in memory and relies on software not reading them, which requires every
access site to remember to check a permission — miss one and there is a silent
hole. Here there is simply **no wire** between the key registers and
`s_axi_rdata`. Software can write a key, mark it locked, erase it, and ask
whether a slot is loaded. To *use* a key, a core in the PL takes it over the
`use` port, and that bundle never leaves the chip.

| Offset | Name | Access | Description |
|---|---|---|---|
| `0x00` | `VERSION` | R | Constant `0x0001_0000` |
| `0x04` | `CTRL` | W | `[0]` ZEROIZE — write 1 for a global erase, self-clearing |
| `0x08` | `STATUS` | R | `[0]` READY, `[1]` TAMPER_LATCHED, `[2]` DENY (an operation was refused; latched, cleared by ZEROIZE) |
| `0x0C` | `SLOT_SEL` | RW | `[2:0]` slot for the current operation |
| `0x10` | `KEY_IN` | W | One 32-bit word; the word index advances automatically. **Reads as 0** |
| `0x14` | `SLOT_CTRL` | W | `[0]` BEGIN, `[1]` COMMIT, `[2]` LOCK, `[3]` ERASE (write 1 to trigger) |
| `0x18` | `SLOT_STAT` | R | `[0]` VALID, `[1]` LOCKED, `[7:4]` words written so far |
| `0x1C` | `VALID_MAP` | R | One bit per slot |
| `0x20` | `LOCK_MAP` | R | One bit per slot |
| `0x24` | `ZERO_CNT` | R | `[7:0]` global erases, saturating |
| `0x28` | `VIOL_CNT` | R | `{read violations[31:16], write violations[15:0]}`, saturating |
| `0x2C` | `VIOL_INFO` | R | `[7:0]` first violation address, `[8]` was a write, `[9]` NS bit, `[10]` valid |
| `0x30` | `PARAM0` | R | `{WORDS[15:8], SLOTS[7:0]}`, for driver self-check |

**No address in this table returns key material.** `KEY_IN` is write-only and
reads as 0 — not "gated to 0", but never connected to the key registers in the
first place. `test_key_never_readable` verifies this by loading all eight slots
with distinct keys and sweeping the entire 256-byte address space word by word,
asserting that none of the 64 key words appears anywhere. The test bites: wiring
one key word into the read multiplexer makes it report the leaking address and
value immediately.

Loading a key:

```
SLOT_SEL  = n            select the slot
SLOT_CTRL = BEGIN        clear that slot's word index and VALID
KEY_IN    × 8            eight 32-bit words (256 bits)
SLOT_CTRL = COMMIT       set VALID
SLOT_CTRL = LOCK         optional; afterwards it can be neither written nor erased
```

**A half-loaded key is not a key**: only `COMMIT` makes a slot usable. **Erase
completes in one cycle** — the whole vault, not a word-by-word sweep — so there
is no window in which half a key survives. **Tamper is one-way**: it can erase,
never un-erase.

## `sym_axi` — slot 2

Window `0x00`–`0x7F`. RTL `hardware/rtl/bus/sym_axi.v`.

**There is no `KEY` register in this table, deliberately.** The usual design has
software write a key into key registers for the cipher core to read — which puts
the key on the bus and leaves it sitting in a register, so anyone who can read
that range has the key. Here software writes only `KEY_SLOT`, a 3-bit slot
number; the key itself arrives from `key_vault`'s `use` port on 256 wires that
never leave the PL. Software says "use the key in slot 3", never "the key is
0x…". "Read the key" therefore has **no address on the bus**, rather than an
address that is gated.

| Offset | Name | Access | Description |
|---|---|---|---|
| `0x00` | `VERSION` | R | Constant |
| `0x04` | `CTRL` | W | `[0]` ZEROIZE (write-1, self-clearing; erases all three cores) |
| `0x08` | `STATUS` | R | `[0]` BUSY, `[1]` DONE, `[2]` KEY_READY, `[3]` KV_VALID |
| `0x0C` | `ALG` | RW | `[1:0]` 0 = AES-128, 1 = AES-256, 2 = SM4, 3 = SM3; `[2]` DECRYPT |
| `0x10` | `KEY_SLOT` | RW | `[2:0]` which key vault slot to use |
| `0x14` | `CMD` | W | `[0]` LOAD_KEY, `[1]` BLOCK, `[2]` HASH_START, `[3]` HASH_FINAL |
| `0x18` | `HASH_IN` | W | `[7:0]` one byte into SM3 |
| `0x1C` | `VIOL_CNT` | R | `{read[31:16], write[15:0]}` |
| `0x20`–`0x2C` | `DIN0..3` | W | Input block; `DIN0` is the most significant 32 bits |
| `0x30`–`0x3C` | `DOUT0..3` | R | Output block |
| `0x40`–`0x5C` | `DIGEST0..7` | R | SM3 digest |
| `0x70` | `PARAM0` | R | Supported-algorithm bitmap |

## `mlkem_axi` — slot 3

RTL `hardware/rtl/bus/mlkem_axi.v`, tests
`hardware/tb/cocotb/test_mlkem_axi.py`.

| Offset | Name | Access | Description |
|---|---|---|---|
| `0x00` | `VERSION` | R | Constant `0x0001_0000` |
| `0x04` | `CTRL` | W | START, ZEROIZE |
| `0x08` | `STATUS` | R | `[0]` BUSY, `[1]` DONE, `[2]` HASH_OK, `[3]` TAMPER, `[4]` WIPING, `[5]` PARAM_ERR |
| `0x0C` | `MODE` | RW | `[1:0]` operation (0 KeyGen, 1 Encaps, 2 Decaps), `[3:2]` parameter set (0 = 512, 1 = 768, 2 = 1024) |
| `0x10` | `IN_DATA` | W | Input byte stream; the write pointer advances automatically |
| `0x14` | `IN_PTR` | RW | Input write pointer |
| `0x18` | `OUT_DATA` | R | Output byte stream |
| `0x1C` | `OUT_LEN` | R | Output length, written by hardware |
| `0x20` | `OUT_RD` | RW | Output read pointer |
| `0x24` | `VIOL_CNT` | R | Firewall violation counters |
| `0x28` | `PARAM0` | R | Capability word |

**Everything goes in through one buffer, in the order the standard defines it**,
because the three cores' input shapes are entirely different and one register
group per shape would produce three unrelated register tables and three copies
of the software marshalling:

```
KeyGen : d(32) ‖ z(32)
Encaps : m(32) ‖ ek(384k+32)
Decaps : dk(768k+96) ‖ c(32·(du·k+dv))
```

The 256-bit quantities that must be presented in parallel (`d`, `z`, `m`) are
lifted out of the head of the buffer by this module, so software never has to
know which inputs are streams and which are parallel ports.

**All lengths are computed in RTL from `pset`.** Software does not report a
length, so it cannot report a wrong one — which would otherwise be an input that
silently produces a wrong answer.

**`mode` and `pset` are 2 bits, but only 0/1/2 exist.** The value 3 is not
another configuration; it is a thing that does not exist. Writing it sets
`PARAM_ERR`, `BUSY` never rises, and any previous `DONE`/`OUT_LEN` are
invalidated. Reads remain permitted so that software can still poll.

**Zeroize really erases the BRAM.** An earlier version cleared only
`in_ptr`/`out_len`/`out_rd`/`seed`. From software nothing was readable
(`OUT_LEN = 0`), but every byte of the previous operation's `dk` was still in
the two 8 KB BRAMs — that is tearing out the index while the text remains.
Every path back was still open: the next operation only overwrites the range it
uses, leaving the old private key in the tail; bitstream readback or a scan
chain could recover it; or one could simply push `in_ptr` into the old region
and start an operation. So there is a real erase machine — triggered on the
**rising edge** of tamper or zeroize, both BRAMs written to zero address by
address in parallel, 8192 cycles, with `WIPING = 1` and output reads refused
throughout.

**What does and does not reach the buffer.** ML-KEM's `dk` is genuinely
returned to software — it is the protocol's private key, which the module wraps
and stores externally — so private key bytes really are readable from
`OUT_DATA`. That is the interface definition, not a leak. The boundary is that
**no intermediate value** (ŝ, ê, Â, r̂, m′, or the re-encrypted c′) ever enters
the buffer; they exist only in BRAM inside the cores. `DEBUG_BANK` is hard-wired
to 0 here, so the polynomial storage read port is not brought out at all, and a
single tamper wire takes down all three cores and both buffers. Software gets
exactly the bytes the algorithm definition says it should get. (Whether `dk`
*should* be returned at all is discussed in
[SECURITY.md](SECURITY.md#limitations).)

## `pqc_accel_axi` — simulation and host path

This is the slave behind `include/pqchsm/accel.h`, driven by the host software
stack rather than by the board programs. Control plane AXI4-Lite, data plane
AXI4-Stream. Decoding uses `[4:2]`, so addresses at and above `0x20` alias back.

| Offset | Name | Access | Description |
|---|---|---|---|
| `0x00` | `CTRL` | W | `[0]` START, `[1]` SOFT_RESET |
| `0x04` | `STATUS` | R | `[0]` DONE, `[1]` BUSY, `[2]` ERR |
| `0x08` | `MODE` | RW | Operation code |
| `0x0C` | `PARAM` | RW | Parameter set |
| `0x10` | `IN_LEN` | RW | Input length in bytes |
| `0x14` | `OUT_LEN` | R | Output length, hardware-written |
| `0x18` | `ERRCODE` | R | Failure detail, hardware-written |
| `0x1C` | `VERSION` | R | Constant `0x0001_0000` |

| Code | Operation | `IN_LEN` | `OUT_LEN` |
|---|---|---|---|
| 7 | NTT, forward | 512 | 512 |
| 8 | NTT, inverse | 512 | 512 |
| 9 | Keccak-f[1600] | 200 | 200 |
| 10 | SHAKE / SHA3, full sponge | 0…512 | 1…512 |

Code 10 is the only one using `PARAM`, packing three fields: `[7:0]` domain
suffix (`0x1F` SHAKE, `0x06` SHA3), `[15:8]` rate in bytes (168/136/72, multiple
of 8), `[31:16]` requested output length. Putting the output length here rather
than adding a register keeps the table fixed.

Codes 9 and 10 share one `keccak_f1600`: mode 10 drives the sponge in
`sha3_core`, and mode 9 borrows the permutation underneath through a passthrough
port that is live only while the sponge is idle. Only one command runs at a
time, so there is no contention — and a second permutation core would cost about
3500 LUTs. Mode 10 wipes the sponge before reporting `DONE`, because squeezing
has no natural end (SHAKE output length is arbitrary), so the sponge would
otherwise never return to idle and never lend the permutation out again.

Any other code, or a length not matching the code, completes with `ERR` and
`ERRCODE = 3` ("mode not implemented"). Reporting the failure is deliberate:
silently falling back to a different implementation would make a partially
implemented accelerator look complete.

**Data plane.** 32 bits wide, buffer addressed in words, little-endian to match
software. Each input packet is written from buffer offset 0; after the beat
carrying `TLAST` is accepted the write pointer returns to 0, so software needs
no "reset write pointer" register. `TREADY` is low while `BUSY` is set. On
completion, `OUT_LEN` converted to words becomes readable; raising `TREADY`
drains the result with `TLAST` on the final beat, which resets the read pointer
and drops `TVALID` — a result can be taken exactly once.

```
stream input packet (TLAST on the last beat)
write MODE, IN_LEN
write CTRL = START
poll STATUS until DONE
if ERR:  read ERRCODE
else:    read OUT_LEN, then drain the output stream
```

## Shared conventions

**START self-clears.** Writing 1 starts a command and the hardware clears the
bit in the same cycle, so `CTRL` always reads back 0. Without this, any
read-modify-write of `CTRL` by software would retrigger the command.

**DONE is a latched level, not a pulse.** It is set at completion and stays set
until the next START. Software polls at arbitrary times and would miss a
one-cycle pulse. Clearing is tied to START rather than to a read of `STATUS`, so
reading the register can never destroy the state it reports.

**ERR accompanies DONE**, in the same cycle, with detail in `ERRCODE`. There is
no separate failure path to poll.

**BUSY reflects the datapath**, not a latched bit: high from START until
completion.

**Status, length and error registers are hardware-written and software
read-only.** Writes are ignored and answered `OKAY` rather than `SLVERR`: on
AXI4-Lite a write to a read-only register is benign, and an error response
would strand masters that do not know the convention.

**Refused accesses have no side effect.** In particular a refused read never
pops the TRNG FIFO — otherwise a non-secure read that gets no data could still
drain the entropy pool.

**DECERR, never OKAY-with-zero.** Silently returning zeros would let the normal
world believe it had read something.
