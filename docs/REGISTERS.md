**English** · [中文](REGISTERS.zh-CN.md)

# Register reference

The contract between software and every AXI slave in the design. Both sides are
written against this document, and each clause is verified individually by a
cocotb testbench.

All registers are 32 bits and word-aligned. Every slave sits behind
`axi4lite_firewall`; addresses outside a slave's window, and non-secure accesses
to a `SECURE_ONLY=1` instance, **read back zero and have their writes
discarded** (RAZ/WI) — no side effect, and no bus error.

> ### ⚠️ What a refused access looks like: zeros, not an error
>
> **The default bitstream (`zu3eg_hsm.bit`) gives the normal world *zero*
> reachability.** All five functional slaves are built with `SECURE_ONLY=1`, so
> every access from Linux — which always carries `AxPROT[1]=1` — is refused at
> the bus. Only the secure world (EL3, via the BL31 SiP) can drive them.
>
> A refused access is **RAZ/WI**: **the read returns 0, the write is discarded,
> the response is always OKAY, and no bus error is raised.** Consequences:
>
> * **To tell whether an access was refused, look at the data, not the response
>   code.** Every core's `VERSION` reads `0x0001_0000`, so a `0` means refused
>   (or no bitstream loaded).
> * **Nothing you write can break the board.** No user-space program, no driver,
>   and no mistyped address can make the kernel see a bus error. This is
>   established in simulation (`test_xbar`, `test_firewall`, `test_trng_axi`)
>   **and on silicon**: `hsm_nocrash` hammers nine classes of address with 2000
>   reads and 2000 writes each — 36,000 accesses — and the board stays up
>   (`board/logs/RESULT_nocrash.txt`).
> * **The cost is that a wrong address is now silent.** It reads 0 instead of
>   faulting. The compensation is the violation counters (`VIOL` on each core,
>   `XBAR_VIOL` for the decoder) — but **only the secure world can read them**;
>   from the normal world those two addresses also read 0.
>
> #### Why not DECERR
>
> DECERR is the "correct" AXI semantic and was what the first version did. It
> was replaced because of an asymmetry: a refused *read* is a synchronous abort
> that Linux turns into `SIGBUS`, which a program can catch, whereas a refused
> *write* is **posted** — the error comes back later as an **SError**, belongs
> to no instruction, and the kernel can only answer it with a panic. **The cost
> is a power cycle**, and on this board a power cycle clears `CSU_MULTI_BOOT`.
>
> Triggering it does not take malice. It happened once for real: the device tree
> still described the factory PL's GPIO, so after loading the crypto bitstream
> the kernel's own `xgpio_of_probe` reached for a peripheral that wasn't there
> and panicked on the spot. **A crypto module should not have the property that
> one slip in user space costs a power cycle**, so this changed.
>
> Security is unchanged: writes still never reach a slave, reads still never
> return key material — word for word what DECERR gave. Only the bus error is
> gone.
>
> #### `PQC_DEV_OPEN=1` now exists for one reason only
>
> ```
> PQC_DEV_OPEN=1 vivado -mode batch -source hardware/syn/impl_bitstream.tcl
> ```
>
> That build sets `SECURE_ONLY=0` on the functional slaves (producing
> `zu3eg_hsm_dev.bit`) so the **normal world can run the KATs directly**, which
> is convenient for debugging. It is no longer needed to avoid crashing on a
> refused write — that restriction is gone. It is a debug form, not the shipping
> one.

- [Slot map](#slot-map)
- [`trng_axi`](#trng_axi--slot-0)
- [`key_vault_axi` (symmetric key registers)](#key_vault_axi--slot-1)
- [`sym_axi`](#sym_axi--slot-2)
- [`mlkem_axi`](#mlkem_axi--slot-3)
- [`mldsa_axi`](#mldsa_axi--slot-6)
- [`pqc_accel_axi`](#pqc_accel_axi--simulation-and-host-path)
- [Shared conventions](#shared-conventions)

## Slot map

Base `0x8000_0000`, slot from `addr[18:16]`, 64 KB each. See
[ARCHITECTURE.md](ARCHITECTURE.md#address-map) for why the decode is exact.

| Slot | Address | Slave | `SECURE_ONLY` (secure form) | `SECURE_ONLY` (demo form) |
|---|---|---|---|---|
| 0 | `0x8000_0000` | `trng_axi` | **1** | 0 |
| 1 | `0x8001_0000` | `key_vault_axi` | **1** | 0 |
| 2 | `0x8002_0000` | `sym_axi` | **1** | 0 |
| 3 | `0x8003_0000` | `mlkem_axi` | **1** | 0 |
| 4 | `0x8004_0000` | canary (`key_vault_axi`) | **1** | **1** |
| 5 | `0x8005_0000` | `fan_ctrl_axi` | 0 | 0 |
| 6 | `0x8006_0000` | `mldsa_axi` | **1** | 0 |

> **⚠️ This table used to say `0` for the five functional slaves and flatly
> contradicted the paragraph three screens above it**, which states that the
> default bitstream gives the normal world zero reachability. The `0` column was
> left over from the *first* build, where only the canary was gated — that build
> proved AxPROT gating works, but proved it only about an empty shell.
>
> The parameter is one build-time knob (`SECURE_ONLY_FUNCTIONAL` in
> `zu3eg_hsm_top.v`) and there are **two** forms, so a single column cannot
> describe it. The canary stays at 1 in both — it is the control group.
> Slot 5 (fan) is never gated: it is not inside the crypto boundary.
>
> ⚠️ Two things do **not** follow this knob: the **seed staging port**
> (`mlkem_axi 0x38`, `mldsa_axi 0x34`) and the **`key_vault_axi` load port**
> (`SLOT_SEL 0x0C`, `KEY_IN 0x10`, `SLOT_CTRL 0x14`) require `AxPROT[1]=0` in
> *both* forms. A demo bitstream is open to the normal world for everything
> else, but seeds and symmetric keys still only come from the secure world.

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
| `0x30` | `RAW` | R | Raw noise tap, non-empty only when `RAW_TAP=1` |
| `0x34` | `DROPS` | R | Sample-FIFO overflow count |
| `0x38` | `VIOL` | R | `{refused reads[31:16], refused writes[15:0]}`, saturating |

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

The symmetric key registers: 8 slots (0–6 for business keys, 7 only for public
KAT keys), each holding one 256-bit key for the duration of a session. Keys are
generated and wrapped by the OP-TEE TA; the PL keeps no long-term key. Window
`0x00`–`0x3F`; anything beyond reads 0 and discards writes. RTL
`hardware/rtl/bus/key_vault_axi.v`, tests
`hardware/tb/cocotb/test_key_vault.py`.

There is **no wire** between the key registers and `s_axi_rdata`. Software can
write a key (secure world only), mark it locked, erase it, and ask whether a
slot is loaded. To *use* a key, a core in the PL takes it over the `use` port,
and that bundle never leaves the chip. All slots are wiped at session end and on
tamper.

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
| `0x34` | `LOAD_REJ` | R | `[15:0]` non-secure writes refused at the load port (`SLOT_SEL`/`KEY_IN`/`SLOT_CTRL`), saturating |

**The load port is secure-only, regardless of `SECURE_ONLY`.** `SLOT_SEL`,
`KEY_IN` and `SLOT_CTRL` accept only `AxPROT[1]=0` writes in both bitstream
forms; a non-secure write is RAZ/WI and increments `LOAD_REJ`. `CTRL.ZEROIZE`
(`0x04`) is deliberately **not** gated — wiping is the safe direction. On the
EL3 side, BL31 removes slot 1 from the generic `PL_RD`/`PL_WR` SiP whitelist
entirely (both reads and writes, both worlds); keys reach these registers only
via TA → PTA → SiP `0x8200ff16` (`PQC_SYMKEY`, secure callers only).

**No address in this table returns key material.** `KEY_IN` is write-only and
reads as 0 — not "gated to 0", but never connected to the key registers in the
first place. `test_key_never_readable` verifies this by loading all eight slots
with distinct keys and sweeping the entire 256-byte address space word by word,
asserting that none of the 64 key words appears anywhere. The test bites: wiring
one key word into the read multiplexer makes it report the leaking address and
value immediately.

Loading a key (performed by the secure world — the TA drives it through
`PQC_SYMKEY` BEGIN/WORD/COMMIT; the normal world cannot):

```
SLOT_SEL  = n            select the slot
SLOT_CTRL = BEGIN        clear that slot's word index and VALID
KEY_IN    × 8            eight 32-bit words (256 bits)
SLOT_CTRL = COMMIT       set VALID
SLOT_CTRL = LOCK         optional; afterwards it can be neither written nor erased
```

**A half-loaded key is not a key**: only `COMMIT` makes a slot usable. **Erase
completes in one cycle** — all slots at once, not a word-by-word sweep — so there
is no window in which half a key survives. **Tamper is one-way**: it can erase,
never un-erase.

## `sym_axi` — slot 2

Window `0x00`–`0x7F`. RTL `hardware/rtl/bus/sym_axi.v`.

**There is no `KEY` register in this table, deliberately.** The usual design has
software write a key into key registers for the cipher core to read — which puts
the key on the bus and leaves it sitting in a register, so anyone who can read
that range has the key. Here software writes only `KEY_SLOT`, a 3-bit slot
number; the key itself arrives from `key_vault_axi`'s `use` port on 256 wires that
never leave the PL. Software says "use the key in slot 3", never "the key is
0x…". "Read the key" therefore has **no address on the bus**, rather than an
address that is gated.

| Offset | Name | Access | Description |
|---|---|---|---|
| `0x00` | `VERSION` | R | Constant |
| `0x04` | `CTRL` | W | `[0]` ZEROIZE (write-1, self-clearing; erases all three cores **and all `key_vault_axi` slots**, via an internal wire — it is not a bus write to slot 1) |
| `0x08` | `STATUS` | R | `[0]` BUSY, `[1]` DONE, `[2]` KEY_READY, `[3]` KV_VALID |
| `0x0C` | `ALG` | RW | `[1:0]` 0 = AES-128, 1 = AES-256, 2 = SM4, 3 = SM3; `[2]` DECRYPT |
| `0x10` | `KEY_SLOT` | RW | `[2:0]` which `key_vault_axi` slot to use |
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
| `0x04` | `CTRL` | W | `[0]` START, `[1]` ZEROIZE, `[2]` IN_RST, `[3]` OUT_RST, `[5]` SEED_LOCK (one-way; cleared only by a PL reset, the daemon does not set it), `[6]` SEED_CLR. `[4]` was DK_LOCK and was removed (V-04); writing it has no effect |
| `0x08` | `STATUS` | R | `[0]` BUSY, `[1]` DONE, `[2]` HASH_OK, `[3]` TAMPER, `[4]` WIPING, `[5]` PARAM_ERR, `[6]` SEED_ERR |
| `0x0C` | `MODE` | RW | `[1:0]` operation (0 KeyGen, 1 Encaps, 2 Decaps), `[3:2]` parameter set (0 = 512, 1 = 768, 2 = 1024), `[4]` CHAIN (expand the private key from the staged seed first, then run the operation, then wipe), `[10]` SEED_STAGED (KeyGen takes `d‖z` from `SEED_DATA`, not `IN_DATA`). Reads back the whole word. `DK_TO_SLOT`, `DK_FROM_SLOT` and `SLOT[9:6]` were removed with the slot ABI: writes to those bits are ignored and read back 0 |
| `0x10` | `IN_DATA` | W | Input byte stream; the write pointer advances automatically |
| `0x14` | `IN_PTR` | RW | Input write pointer |
| `0x18` | `OUT_DATA` | R | Output byte stream |
| `0x1C` | `OUT_LEN` | R | Output length, written by hardware |
| `0x20` | `OUT_RD` | RW | Output read pointer |
| `0x24` | `VIOL_CNT` | R | Firewall violation counters |
| `0x28` | `PARAM0` | R | Capability word |
| `0x2C` | `XBAR_VIOL` | R | **Decoder** violations — accesses that hit no slot at all |
| `0x30` | `KEYSTAT` | R | `[0]` EXP_WIPING (the expansion region is being wiped; software waits for it to clear before the next START), `[1]` SEED_LOCK. No key-related state is reported |
| `0x34` | `KEYPSET` | R | Always 0 (per-slot parameter sets were removed with the slots) |
| `0x38` | `SEED_DATA` | W | Secure-world seed staging: 16 little-endian 32-bit words = `d‖z`. **Only accepts `AxPROT[1]=0`, regardless of `SECURE_ONLY`; there is no read-back path (reads return 0); consumed at START** |
| `0x3C` | `SEED_STAT` | R | `[4:0]` words staged (0..16), `[8]` SEED_READY, `[9]` SEED_LOCK, `[10]` SEED_STAGED, `[11]` SEED_OVF (a write after the 16 words were full, latched), `[31:16]` non-secure writes refused (saturating). **No seed byte appears here** |
| `0x40` | `IN_DATA4` | W | Four input bytes in one write (little-endian: `wdata[7:0]` lands first); advances the write pointer by 4 |
| `0x44` | `OUT_DATA4` | R | Four output bytes in one read; returns 0 and does not advance when fewer than 4 bytes remain |

`MODE` reads back all of its fields, so a driver can check what it wrote.

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

**Zeroize really erases the BRAM.** Clearing only the pointers would leave the
previous operation's `dk` in the two 8 KB BRAMs, recoverable by bitstream
readback, a scan chain, or by pushing `in_ptr` into the old region and starting
an operation. So there is a real erase machine, triggered on the **rising edge**
of tamper or zeroize: both BRAMs are written to zero address by address in
parallel, with `WIPING = 1` and output reads refused throughout.

> **Cycle counts.** The wipe counter is 13 bits: this layer's machine runs
> **8192 cycles** (about 109 µs at 75 MHz). Software polling `WIPING` should size
> its timeout against that. The three ML-KEM cores have their own erase machines
> and `STATUS[4]` is the OR of this layer's machine and all three cores'. Measured
> cycle counts on the board are still pending; the figures above are what the RTL
> counts.

**Expansion region wipe (`KEYSTAT[0]`).** With `MODE.CHAIN`, the private key is
expanded from the staged seed into a 4 KB expansion region inside the PL, used by
the operation, and wiped by hardware at the end of every operation (4096 cycles).
The region has no slot number and no valid bit, so nothing can refer to a
previous operation's contents. START is refused while `KEYSTAT[0]` is set, so
software waits for it to clear after each operation.
`CHAIN` needs a complete staged seed; without one START is refused with
`PARAM_ERR`. START is also refused (with `PARAM_ERR`) while `KEYSTAT[0]` is set.

**What does and does not reach the buffer.** Without `CHAIN` (the ACVP path,
where software supplies `d‖z`), ML-KEM's `dk` is returned to software through
`OUT_DATA`; that is the interface definition. With `CHAIN` on KeyGen, `dk` goes
only to the expansion region and `OUT_LEN` covers `ek` only. The boundary is that
**no intermediate value** (ŝ, ê, Â, r̂, m′, or the re-encrypted c′) ever enters
the buffer; they exist only in BRAM inside the cores. `DEBUG_BANK` is hard-wired
to 0 here, so the polynomial storage read port is not brought out at all, and a
single tamper wire takes down all three cores and both buffers. Software gets
exactly the bytes the algorithm definition says it should get. (Whether `dk`
*should* be returned at all is discussed in
[SECURITY.md](SECURITY.md#limitations).)

## `mldsa_axi` — slot 6

RTL in `hardware/rtl/bus/mldsa_axi.v`, tests in
`hardware/tb/cocotb/test_mldsa_axi.py` (judged against the official ACVP
vectors and `hardware/model/mldsa_oracle.py`).

| Offset | Name | Access | Notes |
|---|---|---|---|
| `0x00` | `VERSION` | R | Constant `0x0001_0000` |
| `0x04` | `CTRL` | W | `[0]` START, `[1]` CLEAR, `[2]` ZEROIZE, `[5]` SEED_LOCK (one-way latch; cleared only by a PL reset), `[6]` SEED_CLR. `[4]` was SK_LOCK and was removed (V-04); writing it has no effect |
| `0x08` | `MODE` | RW | `[1:0]` op (0 KeyGen, 1 Sign, 2 Verify), `[3:2]` parameter set (0 = 44, 1 = 65, 2 = 87), `[4]` CHAIN (Sign: expand sk from the staged ξ, sign, then wipe; KeyGen: return only pk), `[10]` SEED_STAGED (KeyGen takes ξ from `SEED_DATA`, not `IN_DATA`). Reads back the whole word. `SK_TO_SLOT`, `SK_FROM_SLOT` and `slot[9:6]` were removed with the slot ABI. Writing `MODE` resets `IN_PTR` to 0 |
| `0x0C` | `STATUS` | R | `[0]` BUSY, `[1]` DONE, `[2]` VERIFY_OK, `[3]` PARAM_ERR, `[4]` LEN_ERR, `[5]` TAMPER, `[6]` WIPING, `[7]` SEED_ERR (a staged-seed KeyGen was started with no complete ξ staged) |
| `0x10` | `IN_DATA` | W | Input byte stream; the write pointer advances |
| `0x14` | `IN_PTR` | RW | Input write pointer. **Only zero may be written** — an arbitrary setting would be a path around the "enough bytes fed" check |
| `0x18` | `OUT_DATA` | R | Output byte stream; bytes are handed over only while `OUT_PTR < OUT_LEN`, otherwise zero |
| `0x1C` | `OUT_PTR` | RW | Output read cursor, freely writable (reads are still bounded by `OUT_LEN`) |
| `0x20` | `OUT_LEN` | R | Output length, written by hardware |
| `0x24` | `MSG_LEN` | RW | Message bytes, at most 8192 (the core's `u_msg` is AW=13) |
| `0x28` | `CTX_LEN` | RW | FIPS 204 context string length, at most 255 |
| `0x2C` | `KEYSTAT` | R | `[0]` EXP_WIPING (the expansion region is being wiped; wait for it to clear before the next START), `[1]` SEED_LOCK. No key-related state is reported |
| `0x30` | `VIOL` | R | `[15:0]` write violations, `[31:16]` read violations |
| `0x34` | `SEED_DATA` | W | Secure-world ξ staging: 8 little-endian 32-bit words. **Only accepts `AxPROT[1]=0`, regardless of `SECURE_ONLY`; reads return 0; consumed at START** |
| `0x38` | `SEED_STAT` | R | `[3:0]` words staged (0..8), `[8]` SEED_READY, `[9]` SEED_LOCK, `[10]` SEED_STAGED, `[11]` SEED_OVF, `[31:16]` non-secure writes refused (saturating). No seed byte appears here |
| `0x40` | `IN_DATA4` | W | Four input bytes in one write (little-endian) |
| `0x44` | `OUT_DATA4` | R | Four output bytes in one read; returns 0 and does not advance when fewer than 4 bytes remain |

**⚠️ `MODE` is at `0x08` and `STATUS` at `0x0C` — the opposite of `mlkem_axi`.**
The two were once swapped in the daemon, and the symptom was deceptive:
`VERSION`, `IN_DATA` and `IN_PTR` all sit outside the swapped pair, so "the
version reads back and `IN_PTR` matches after feeding" both held and the path
looked healthy. In reality `MODE` was written into the read-only `STATUS` (the
core never received op/pset), and the polled "`STATUS`" was `MODE`, which never
shows `DONE`. It presents as "every run times out waiting for done", which is
indistinguishable from "the core cannot compute it". **Write against this table,
not from memory.**

**Input layout** (same reasoning as ML-KEM: one byte port, order defined by the
standard):

```
KeyGen : xi(32)
Sign   : [sk(skLen), only when CHAIN=0] || rnd(32) || ctx(ctx_len) || msg(msg_len)
Verify : pk(pkLen) || sig(sigLen) || ctx(ctx_len) || msg(msg_len)
```

`rnd` is the FIPS 204 hedged-mode randomness. **All zeros means deterministic
signing**, which makes the signature a pure function of the message and removes
a layer of fault-injection and side-channel defence; determinism is only needed
when matching ACVP's deterministic vectors.

**Expansion region (`CHAIN`).** The PL does not store signing keys. With
`CHAIN`, the TA first writes ξ to the staging port; the layer expands sk into an
8 KB expansion region (no slot number, no valid bit), copies it into the engine,
signs from `rnd‖ctx‖msg`, and hardware wipes the region at the end of the
operation. `OUT_LEN` covers σ only, and seeking the read cursor into the sk
region yields nothing. KeyGen with `CHAIN` returns only pk. Without `CHAIN`
(the ACVP path, software supplies ξ or sk) KeyGen returns `pk‖sk` and Sign takes
sk from the input stream. `SEED_LOCK`, once set, forces KeyGen to use the staged
seed; there is **no clearing path**: ZEROIZE does not clear it, only a PL reset
does.

**Checks before START** (on failure `PARAM_ERR`/`LEN_ERR` are set and the engine
**does not start at all**): `op` and `pset` are only meaningful as 0/1/2;
`ctx_len <= 255`; `msg_len <= 8192`; software fed enough bytes; the total
fits the input buffer; a complete staged ξ exists when KeyGen uses one (otherwise
`SEED_ERR` as well) or when `CHAIN` is set; the expansion region is not still
being wiped. **Whether or not the START is accepted, the previous
`DONE`/`OUT_LEN`/`VERIFY_OK` are invalidated on the spot** — without that,
software polls a stale `DONE` and reads the previous run's output as this run's
result, which looks like success.

**In the secure form this slot must be in the BL31 SiP whitelist.** With
`SECURE_ONLY=1` the normal world cannot touch a single register; the daemon goes
through `/dev/secmmio` to EL3. Without the `0x8006_0000` row the whole core is
effectively absent in that form (see `boot/atf/patch_atf_secmmio.py`).

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

**RAZ/WI, not DECERR — and this used to say the opposite.** The old rule here
read *"DECERR, never OKAY-with-zero: silently returning zeros would let the
normal world believe it had read something."* That worry is real and it has not
gone away; it was outweighed. A DECERR on a *posted write* returns as an SError
and panics the kernel, so under the old rule any mistyped address cost a power
cycle. Un-crashability won.

Three things keep the old worry in check:

* **`STATUS` reads 0 too**, so `ENABLED` and `READY` are both clear — any
  consumer that checks readiness before taking data (which is the correct way to
  write one anyway) sees "not ready" and never reaches the zeros.
* **`VERSION` reads 0**, and it is a nonzero constant on every core. That is the
  loudest available "you were refused" signal, and it is what the boundary
  counter-proof now keys on.
* **The violation counters record it**, readable only from the secure world.

What is genuinely lost: a consumer that checks neither readiness nor version and
just reads `RDATA` now gets zeros instead of an error. Such a consumer was
already broken — it would mishandle a real underrun the same way — but the
failure has moved from "the bus stops you" to "you have to write it correctly."
That is a real trade, stated here rather than buried.
