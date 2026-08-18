**English** · [中文](ARCHITECTURE.zh-CN.md)

# Architecture

How the module is put together, and why the boundaries fall where they do.
Register-level detail is in [REGISTERS.md](REGISTERS.md); what the boundary is
claimed to guarantee is in [SECURITY.md](SECURITY.md).

- [Topology](#topology)
- [Address map](#address-map)
- [Clocking and reset](#clocking-and-reset)
- [Cryptographic cores](#cryptographic-cores)
- [Key vault and firewall](#key-vault-and-firewall)
- [Entropy source](#entropy-source)
- [Software stack](#software-stack)
- [Host key hierarchy](#host-key-hierarchy)
- [Integration notes](#integration-notes)

## Topology

The device is a Zynq UltraScale+ MPSoC: hard ARM cores (PS) and FPGA fabric
(PL) on one die, joined by AXI. Everything cryptographic is in the PL. The PS
runs Linux and issues commands; it holds no key material of its own.

```
   ┌──────────────────────── PS · Cortex-A53 ────────────────────────┐
   │  application ──▶ libsdfe (SDF-style)  ──▶ pqchsm_fpgad          │
   │                                              │                  │
   │  ······················· normal world ·······│················  │
   │                                       /dev/secmmio  →  EL3 SiP  │
   └──────────────────────────────────────────────│──────────────────┘
                       M_AXI_HPM0_LPD  (AXI4, AxPROT[1] = security bit)
                                                  ▼
   ┌──────────────────────── PL · FPGA fabric ───────────────────────┐
   │              axi4lite_xbar  (full decode, one address per reg)  │
   │   ┌──────┬──────────┬──────────┬──────────┬─────────┬────────┐  │
   │   │ slot0│  slot1   │  slot2   │  slot3   │  slot4  │ slot5  │  │
   │   │ trng │key_vault │   sym    │  mlkem   │ canary  │  fan   │  │
   │   └──────┴────┬─────┴────▲─────┴──────────┴─────────┴────────┘  │
   │               └──────────┘  use_key: private wire, not the bus  │
   │      every slot sits behind axi4lite_firewall (AxPROT gate)     │
   │                                                                 │
   │   SYSMONE4 ──▶ fan_ctrl ──▶ pin AA11   (no AXI in this path)    │
   └─────────────────────────────────────────────────────────────────┘
```

For remote calls the `libsdfe` → `pqchsm_fpgad` hop runs over **mTLS** (TLS 1.3,
certificates on both sides); the local hop is a 0600 UNIX socket and is deliberately
not wrapped (see "The remote port" in `docs/SECURITY.md`). **The frame format is
byte-identical on both paths**, so nothing else in the diagram above changes.


Two paths deliberately bypass the bus:

- **`use_key`** — the key vault hands a key to the symmetric cores over private
  wires. There is no bus-visible read path for key material at all; see
  [Key vault and firewall](#key-vault-and-firewall).
- **Fan control** — junction temperature is read from the PL's own SYSMONE4 over
  DRP and drives the fan pin directly. Cooling must work when Linux does not:
  during the first seconds of boot, when U-Boot is sitting at a prompt, and when
  the kernel has hung. The AXI slot 5 interface is observation only; remove it
  and the fan still runs.

## Address map

Base `0x8000_0000`, slot selected by `addr[18:16]`, 64 KB per slot.

| Address | Slave | `SECURE_ONLY` | Purpose |
|---|---|---|---|
| `0x8000_0000` | `trng_axi` | 0 | Entropy source. Reading `0x08` pops one 32-bit word |
| `0x8001_0000` | `key_vault_axi` | 0 | Key vault. Write-only path in; no read path out |
| `0x8002_0000` | `sym_axi` | 0 | AES-128/256, SM4, SM3 |
| `0x8003_0000` | `mlkem_axi` | 0 | ML-KEM 512/768/1024, parameter set from `MODE[3:2]` |
| `0x8004_0000` | canary (`key_vault_axi`) | **1** | Same module as slot 1, differing only in this parameter |
| `0x8005_0000` | `fan_ctrl_axi` | 0 | Fan temperature/duty observation |
| `0x8006_0000` | `mldsa_axi` | per form | ML-DSA-44/65/87, parameter set from `MODE[3:2]`; 8-slot on-chip signing vault |

**The decode is one-to-one: a register has exactly one address that reaches
it.** `axi4lite_xbar` checks the aperture high bits, the slot number, the high
bits of the in-slot offset and 32-bit alignment; all four must hold or the
transaction is refused **in place** — read returns 0, write is discarded — and
the slave sees nothing.

That strictness is not cosmetic. No PS-side protection unit covers this window —
UG1085 v2.5 puts `0x8000_0000` outside XPPU's aperture table and off the
FPD_XMPU path — so the decode plus the firewall in the PL is the *only*
enforcement layer on this route. Mirror addresses would be thousands of
equivalent entrances to the one defence, each returning OKAY and leaving no
trace. `test_xbar` sweeps a whole 64 KB slot word by word: exactly the 64
addresses `0x00`–`0xFC` are reachable, the other 16,320 all read back 0.

**All four functional slaves are `SECURE_ONLY=1`.** Linux runs in the normal
world, so every transaction it issues through `/dev/mem` carries
`AxPROT[1] = 1`. With all four set to 1, Linux cannot read a single register of
the crypto cores.

That choice decides how the KATs are driven, and the alternative is worth naming
because it looks equivalent and is not. Leaving the functional slaves open and
demonstrating the gate on a canary instance elsewhere proves the gate works —
but **what it shows to be protected is the empty canary, not the crypto cores.**
So all four are closed, and the whole KAT suite is driven from the **secure
world** instead — a patched BL31 exposes a
restricted secure-MMIO SiP whose whitelist covers exactly these cores' legal
register offsets (`boot/atf/patch_atf_secmmio.py`), and the normal-world test
program routes every core access through it via SMC. The canary stays at slot 4
as a same-parameter control. `PQC_DEV_OPEN=1` still produces the old open build
(`zu3eg_hsm_dev.bit`) for debugging.

## Clocking and reset

```
pl_clk0 (150 MHz, from PS)
   └─ BUFGCE_DIV ÷2 ──▶ clk_sys (75 MHz) ──▶ all crypto cores + fan + xbar
                                          └─▶ PS maxihpm0_lpd_aclk

pl_resetn0 (async) ──▶ 2-stage synchroniser ──┐
                                              ├─ & ──▶ rst_n
fabric power-on reset (count to 255 after GSR)┘
```

- **75 MHz, not 150.** The slowest core, `mlkem_decaps`, closes at 108.5 MHz
  out-of-context. 75 MHz leaves it 45 % headroom, which is what the clock tree
  and bus integration consume once everything is joined up.
- **`BUFGCE_DIV`, not an MMCM.** An MMCM has a lock to wait for and a reset
  ordering to get right — more state that can go wrong. A pure divider is
  correct from power-on.
- **A fabric-generated power-on reset as well.** After configuration every FPGA
  flip-flop is 0 (GSR), so that counter necessarily starts at 0. With it, the
  design comes up even if `pl_resetn0` is not released cleanly after a runtime
  reconfiguration — one fewer dependency that only shows itself on real silicon.

## Cryptographic cores

Plain inferrable Verilog-2001 throughout: no vendor primitive is instantiated in
any crypto module, so the same sources target Xilinx, Intel or Lattice
unchanged. (Vendor primitives appear only in the board top level, and
`hardware/tb/lint/vendor_stubs.v` supplies shells so that the top level still
passes lint rather than being excluded from it.)

| Directory | Modules | Contents |
|---|---|---|
| `hardware/rtl/mlkem/` | 27 | NTT, base multiply, Montgomery/Barrett reduction, sampling, compression, encode/decode, and whole KeyGen / Encaps / Decaps cores |
| `hardware/rtl/mldsa/` | 13 | ML-DSA operators — NTT, rounding, hints, sampling. Verified, but not chained into whole cores |
| `hardware/rtl/keccak/` | 2 | `keccak_f1600` permutation and the `sha3_core` sponge (G / PRF / XOF / H share one instance) |
| `hardware/rtl/sym/` | 6 | AES-128/256, SM4, SM3 |
| `hardware/rtl/trng/` | 6 | Ring-oscillator source, RCT/APT health tests, SHA-3 conditioning, erasable FIFO, AXI wrapper |
| `hardware/rtl/bus/` | 8 | AXI4-Lite firewall, key vault, per-core AXI wrappers |
| `hardware/rtl/board/` | 2 | `axi4lite_xbar` crossbar and the `zu3eg_hsm_top` board top level |
| `hardware/platform/fan_ctrl/` | 4 | Fan control — **not crypto**, kept in a separate tree; shares only clock and reset |

Algorithm inventory, parameter sets and validation status are tabulated in
[reference/security-policy.md](reference/security-policy.md).

`sha3_core` serves all four Keccak uses in ML-KEM from a single instance.
Decaps instantiates the Encaps core directly for its re-encryption step — that
is not a shortcut, it is what FIPS 203 §7.3 defines the operation to be.

Constant time is non-negotiable in exactly one place: the Decaps implicit-reject
comparison compares **every** byte and then selects, never returning early on
the first difference.

## Key vault and firewall

The invariant is not "the key is hard to read". It is **there is no path**.

Keys are written over the bus into `key_vault`, and leave only over the
`use_key` wires into the symmetric cores. The RTL contains no bus-side read of
the key registers, so no software — including the daemon that loaded the key —
can retrieve one. Related rules:

- **A half-loaded key is not a key.** A key becomes usable only on `COMMIT`;
  partial writes cannot be used.
- **Erase is one cycle.** The whole vault is cleared in a single clock rather
  than swept word by word, so there is no window in which half of a key is still
  present.
- **Tamper is one-way.** A tamper input can erase, and can never un-erase.
- **Violations leave a trace**, so a refused access is distinguishable from one
  that never happened.

`axi4lite_firewall` gates each slave. With `SECURE_ONLY=1` it requires
`AxPROT[1] == 0`; anything else reads back 0 and has its write discarded, with
no bus error (RAZ/WI). Two details matter:

- **Refused reads never pop the FIFO.** Otherwise a non-secure read that gets no
  data could still consume random words — a way for the normal world to drain
  the entropy pool. This matters *more* under RAZ/WI, not less: the response
  code no longer distinguishes allowed from refused, so "the FIFO did not move"
  is the observable that proves the transaction went nowhere.
- **RAZ/WI, not DECERR — and this bullet used to say the opposite.** The old
  rule was *"DECERR, not OKAY-with-zero: refusal has to be visible."* It was
  overruled by a worse failure: a refused **write** is posted, so its DECERR
  comes back as an SError, and the kernel can only panic — one mistyped address,
  one power cycle. The board took that hit once from the kernel's own GPIO
  probe, not from an attacker.

  Refusal is still visible, just not through the response code: every `VERSION`
  is a nonzero constant, so reading `0` *is* the signal, and the violation
  counters (secure-world-readable only) keep the audit trail. What is genuinely
  given up is diagnosability of a mistyped address — traded for the property
  that **no user-space program can take the board down**.

## Entropy source

```
ring-osc array ──sample/decimate──┬──▶ continuous health tests (RCT + APT)
                                  └──▶ Keccak sponge conditioning ──▶ FIFO ──▶ AXI
```

Eight ring oscillators of different lengths (13/15/…/27 stages), two-stage
synchronisers, then decimation.

- **Conditioning is a Keccak sponge, not a home-made whitener.** SP 800-90B
  §3.1.5.1.2 lists vetted conditioning constructions; using one lets output
  entropy be counted as `min(output length, input entropy)` directly, where an
  invented whitener would need its own argument. It also reuses `keccak_f1600`,
  which costs no extra area.
- **Health tests consume the post-decimation stream** — the same stream the
  conditioner consumes. Testing something other than what is used proves
  nothing.
- **An alarm clears the pool too.** Alarm → `ready` low → FIFO erased →
  conditioner and sponge state reset → startup tests re-run. The standard only
  requires stopping output; an alarm means the source may already have been
  degraded for some time, so keeping the pool is riskier than discarding it.
- **Cutoffs come from the measured entropy**, not an assumption. See
  [TESTING.md](TESTING.md#entropy).

## Software stack

```
   PKCS#11 v3.2 shared library          SDF-style library + daemon
   src/p11/                             service/
 ─────────────────────────────────────────────────────────────────
   slot manager     keystore        backup / recovery    audit
   src/slot/        src/store/      src/backup/          src/audit/
 ─────────────────────────────────────────────────────────────────
   pqc_backend_t          include/pqchsm/pqc.h
 ─────────────────────────────────────────────────────────────────
   accel_transport_t      include/pqchsm/accel.h
   stub | Verilator RTL | AXI | /dev/mem + mmap
```

Each layer depends only on the interface below it, and everything above the
`pqc_backend_t` vtable works on opaque handles. **No function declared in
`include/pqchsm/` returns private key material**: key generation returns a
handle, signing takes a handle and a message, decapsulation takes a handle and a
ciphertext. A handle encodes the slot and the slot's generation counter, so
destroying an object invalidates every handle previously issued for it instead
of silently rebinding them to a new key.

The hardware seam existed before any RTL did. `accel.h` fixed the register
contract — `CTRL`, `STATUS`, `MODE`, `PARAM`, `IN_LEN`, `OUT_LEN`, `ERRCODE` —
so interface mistakes surfaced in software first, and four transports implement
it identically: a software stub, Verilator-simulated RTL, that same RTL driven
over real AXI4-Lite and AXI4-Stream, and `/dev/mem` + `mmap` on the board. The
stub and the simulated RTL must produce byte-identical results through it, which
`tests/unit/test_accel.c` asserts.

`accel_shake()` implements the sponge — padding, rate, absorb, squeeze — in C,
calling the transport only for the permutation. A permutation-only core is
smaller and serves SHAKE128, SHAKE256, SHA3-256 and SHA3-512 alike.

> **What has run on silicon.** The ACVP results, the symmetric vectors, the
> entropy capture and the boundary proof come from the standalone programs in
> `board/src/`, and from `service/` above them. The `src/` host stack above
> `accel_transport_t` has **not** been driven against the real programmable
> logic; wiring it up is separate work.

## Host key hierarchy

```
KDR  (device-bound root, 32 bytes — currently a stub)
 └── KEK        wraps key material at rest; cannot leave the device
RMK  (recovery master key, randomly generated)
 └── BEK        wraps backup blobs; portable to a replacement device
      └── Shamir M-of-N shares over GF(256)
```

Two wrapping keys exist because the purposes conflict: material under a
device-bound key is safe but unrecoverable if the device dies, material under a
portable key is recoverable but only as protected as the custody of its shares.
Each slot carries a policy bit for whether its key participates in backup at
all, so a device identity key can be made deliberately unrecoverable.

A keystore file is per-slot blobs plus a whole-file MAC:

```
slot blob := plaintext metadata ‖ AES-256-GCM(KEK, aad = metadata, key material ‖ PIN material)
file      := header ‖ slot blob × N ‖ KMAC256(file)
```

Metadata is authenticated rather than encrypted, so a slot's algorithm, usage
and policy can be read without unwrapping while staying tamper-evident. Writes
are atomic — temporary file → `fsync` → `rename` → `fsync(dir)` — so a crash
leaves either the old file or the new one.

Every state transition appends an audit record whose SHA3-256 hash covers the
previous record's hash. A pure chain does not survive an attacker who rewrites
the whole file, so the head is signed with an ML-DSA device identity key and
anchored outside the device.

**Key injection** provisions a key without exposing it: the provisioning host
encapsulates to the device's own ML-KEM public key and wraps a *seed* under the
resulting shared secret. FIPS 203 and 204 keys are fully determined by their
seed, so this moves fewer sensitive bytes and lets the device verify the
expansion itself.

## Integration notes

`hardware/rtl/board/zu3eg_hsm_top.v` wires everything together in one file.
Three things there were paid for on real hardware:

**`M_AXI_HPM0_LPD` is AXI4, not AXI4-Lite.** Treating it as Lite leaves `bid`,
`rid` and `rlast` — all PL-driven, all returned to the PS — dangling. The PS
waits forever for `rlast` and the CPU hangs on that one load instruction.
Synthesis, place-and-route, timing and bitstream generation are all clean, and
the board even reports "operating". AXI4-Lite is AXI4 with burst length fixed at
1, so `rlast = rvalid` and `bid`/`rid` echo `awid`/`arid`. The implementation
flow now carries a post-synthesis assertion for it.

**Declaration order is not a style question.** Referencing a `wire` declared
later does not error in Vivado — it silently creates a new, undriven net of the
same name at that port. Icarus rejects the same code outright, which is why the
lint flow has vendor stubs: excluding the board top level from lint would
disable the only automatic check that catches this.

**Fan safety always errs towards more airflow.** Minimum duty is 25 % rather
than 0; ≥80 °C forces 100 % and does not release until 74 °C; a stale
temperature forces 100 %, because when the temperature is unknown the only safe
assumption is that it is high. A fourth rule was added after a board run: a
reading that never changes a single bit also forces 100 %. The first SYSMON
configuration was wrong in a way where DRP still answered and the register still
held a plausible 32.5 °C — the ADC simply was not converting. "No reading" never
became true, so the staleness rule could not fire. A real junction temperature
always jitters, so perfect stability is itself the fault signal.
