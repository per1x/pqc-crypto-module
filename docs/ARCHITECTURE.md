**English** · [中文](ARCHITECTURE.zh-CN.md)

# Architecture

How the module is put together and where the boundaries fall. Register-level
detail is in [REGISTERS.md](REGISTERS.md); what the boundary is claimed to
guarantee is in [SECURITY.md](SECURITY.md).

- [Topology](#topology)
- [Address map](#address-map)
- [Clocking and reset](#clocking-and-reset)
- [Cryptographic cores](#cryptographic-cores)
- [Symmetric key registers and firewall](#symmetric-key-registers-and-firewall)
- [Entropy source](#entropy-source)
- [Key management](#key-management)
- [Software stack](#software-stack)
- [Key hierarchy](#key-hierarchy)
- [Integration notes](#integration-notes)

## Topology

The device is a Zynq UltraScale+ MPSoC: hard ARM cores (PS) and FPGA fabric (PL)
on one die, joined by AXI. Keys are managed by an OP-TEE trusted application (TA)
in the secure world on the Cortex-A53. The PL does all cryptographic computation
and the TRNG, and keeps no long-term key. Linux in the normal world runs the
daemon, which serialises hardware access and carries key blobs it cannot open.

```
 ┌──────────────────────────── PS · Cortex-A53 ────────────────────────────┐
 │ application ─▶ libsdfe / hsmctl / hsm_httpd ─▶ pqchsm_fpgad (daemon)    │
 │                                   op sequences │          │ key blobs   │
 │ ···························· normal world ·····│··········│·············│
 │                                     /dev/secmmio     OP-TEE TA (S-EL0)  │
 │                                                │     gen / wrap / unwrap│
 │                                                │     PTA (S-EL1)        │
 │                                                ▼          ▼             │
 │              BL31 SiP (EL3): whitelisted MMIO · seed / sym-key loading  │
 └────────────────────────────────────────┬────────────────────────────────┘
                 M_AXI_HPM0_LPD  (AXI4, AxPROT[1] = security bit)
                                          ▼
 ┌──────────────────────────── PL · FPGA fabric ───────────────────────────┐
 │                axi4lite_xbar  (full decode, one address per reg)        │
 │ ┌───────┬──────────┬─────────┬──────────┬─────────┬─────────┬─────────┐ │
 │ │ slot0 │  slot1   │  slot2  │  slot3   │  slot4  │  slot5  │  slot6  │ │
 │ │ trng  │ sym key  │   sym   │  mlkem   │ canary  │   fan   │  mldsa  │ │
 │ │       │ registers│AES/SM4/ │ 512/768/ │ same as │ observe │44/65/87 │ │
 │ │       │          │   SM3   │   1024   │ slot 1  │  only   │         │ │
 │ └───────┴────┬─────┴────▲────┴──────────┴─────────┴─────────┴─────────┘ │
 │              └──────────┘  key goes to the cipher over a direct wire    │
 │  slots 0-4 and 6 sit behind axi4lite_firewall (AxPROT gate);            │
 │  the fan port (slot 5) is outside the boundary, no firewall             │
 │                                                                         │
 │  SYSMONE4 ──▶ fan_ctrl ──▶ pin AA11   (no AXI in this path)             │
 └─────────────────────────────────────────────────────────────────────────┘
```

Two paths into the PL carry different things. Ordinary operations (register
sequences for the cores) go from the daemon through `/dev/secmmio` to a BL31 SiP
that issues the access from EL3 against a whitelist. Key material goes from the
TA through the pseudo-TA (PTA, S-EL1) to other SiP calls: `0x8200ff16` for
symmetric keys, `0x8200ff15` for post-quantum seed words, `0x8200ff17` for TRNG
words. The whitelist excludes the slot 1 load port and the seed ports, so only
the TA can use them. The SiP list is in [boot/README.md](../boot/README.md).

For remote calls the `libsdfe` to `pqchsm_fpgad` hop runs over mTLS (TLS 1.3,
certificates on both sides, TCP 9797). The local hop is a 0600 UNIX socket
without TLS (see "The remote port" in [SECURITY.md](SECURITY.md)). The frame
format is the same on both paths.

Two paths bypass the bus on purpose:

- `use_key`: the symmetric key registers hand a key to the symmetric cores over
  private wires. The bus has no read path for key material. See
  [Symmetric key registers and firewall](#symmetric-key-registers-and-firewall).
- Fan control: junction temperature is read from the PL's own SYSMONE4 over DRP
  and drives the fan pin directly. Cooling has to work when Linux does not (the
  first seconds of boot, U-Boot at a prompt, a hung kernel). The AXI slot 5
  interface only observes and overrides; without it the fan still runs.

## Address map

Base `0x8000_0000`, slot selected by `addr[18:16]`, 64 KB per slot.

| Address | Slave | `SECURE_ONLY` | Purpose |
|---|---|---|---|
| `0x8000_0000` | `trng_axi` | per bitstream | Entropy source. Reading `0x08` pops one 32-bit word |
| `0x8001_0000` | `key_vault_axi` | per bitstream | Symmetric key registers: 8 entries, 256 bit each (0 to 6 for applications, 7 only for public KAT keys). The load port (`SLOT_SEL`, `KEY_IN`, `SLOT_CTRL`) accepts secure transactions only, whatever `SECURE_ONLY` is; refused writes are dropped and counted in `LOAD_REJ` (`0x34`). No read path |
| `0x8002_0000` | `sym_axi` | per bitstream | AES-128/256, SM4, SM3 |
| `0x8003_0000` | `mlkem_axi` | per bitstream | ML-KEM 512/768/1024, parameter set from `MODE[3:2]`; seed staging port (secure-only) and a per-operation expansion region |
| `0x8004_0000` | canary (`key_vault_axi`) | 1 | Same module as slot 1 with `SECURE_ONLY` fixed at 1. Its `use` port is unconnected |
| `0x8005_0000` | `fan_ctrl_axi` | no firewall | Fan temperature, duty and override. Outside the cryptographic boundary |
| `0x8006_0000` | `mldsa_axi` | per bitstream | ML-DSA-44/65/87, parameter set from `MODE[3:2]`; seed staging port (secure-only) and a per-operation expansion region |

"Per bitstream" means `SECURE_ONLY=1` in the default bitstream `zu3eg_hsm.bit` and
`SECURE_ONLY=0` in the development bitstream `zu3eg_hsm_dev.bit` (see below). The
PL holds no signing vault: an ML-DSA private key is expanded from a seed for each
operation and wiped by the hardware afterwards.

The decode is one-to-one: a register has exactly one address that reaches it.
`axi4lite_xbar` checks the aperture high bits, the slot number, the high bits of
the in-slot offset and 32-bit alignment. If any of them fails, the transaction is
refused in place (a read returns 0, a write is discarded) and the slave sees
nothing.

No PS-side protection unit covers this window. UG1085 v2.5 puts `0x8000_0000`
outside XPPU's aperture table and off the FPD_XMPU path, so the decode plus the
firewall in the PL is the only enforcement on this route. Mirror addresses would
add equivalent entrances to that one defence, each returning OKAY and leaving no
trace. `test_xbar` sweeps a whole 64 KB slot word by word: exactly the 64 addresses
`0x00` to `0xFC` are reachable and the other 16,320 read back 0.

**Default and development bitstreams.** Both come from the same RTL. In the default
bitstream the five functional slaves (slots 0, 1, 2, 3, 6) are `SECURE_ONLY=1`, so
Linux, whose transactions carry `AxPROT[1]=1`, reads 0 at every register. Everything
the daemon does goes through `/dev/secmmio` and the BL31 whitelist
(`boot/atf/patch_atf_secmmio.py`). `PQC_DEV_OPEN=1` builds `zu3eg_hsm_dev.bit` with
the functional slaves at `SECURE_ONLY=0`, so Linux can drive them through
`/dev/mem`. The symmetric-key load port and the seed ports stay secure-only in the
development bitstream too. The board currently boots the development bitstream:
`board/scripts/hsm-boot.sh` loads `zu3eg_hsm_dev.bit`, and the default bitstream
is loaded at run time with `pl_deadman.sh` to check the boundary. The daemon uses
the same path (`/dev/secmmio`) with either bitstream.

The canary at slot 4 is the control. The functional slaves cannot show the gate
working if they are open, and a closed canary shows it without hiding a broken core:
`VERSION` is a nonzero constant, so reading 0 there means the firewall refused the
access.

A new slave has to be added to the BL31 whitelist as well. Without its row the core
cannot be reached in the default bitstream: the bitstream loads, the core is there,
and every read returns nothing.

## Clocking and reset

```
pl_clk0 (150 MHz, from PS)
   └─ BUFGCE_DIV ÷2 ──▶ clk_sys (75 MHz) ──▶ all crypto cores + fan + xbar
                                          └─▶ PS maxihpm0_lpd_aclk

pl_resetn0 (async) ──▶ 2-stage synchroniser ──┐
                                              ├─ & ──▶ rst_n
fabric power-on reset (count to 255 after GSR)┘
```

- 75 MHz rather than 150. The slowest core, `mlkem_decaps`, closes at 108.5 MHz
  out-of-context. 75 MHz leaves it 45 % headroom for clock tree and bus
  integration.
- `BUFGCE_DIV` instead of an MMCM. An MMCM has a lock to wait for and a reset order
  to get right; a divider is correct from power-on.
- A fabric-generated power-on reset as well. After configuration every flip-flop is
  0 (GSR), so the counter starts at 0. The design comes up even if `pl_resetn0` is
  not released cleanly after a runtime reconfiguration.

## Cryptographic cores

Plain inferrable Verilog-2001: no vendor primitive is instantiated in any crypto
module, so the same sources target Xilinx, Intel or Lattice. Vendor primitives
appear only in the board top level, and `hardware/tb/lint/vendor_stubs.v` supplies
shells so that the top level still passes lint.

| Directory | Modules | Contents |
|---|---|---|
| `hardware/rtl/mlkem/` | 27 | NTT, base multiply, Montgomery/Barrett reduction, sampling, compression, encode/decode, and whole KeyGen / Encaps / Decaps cores |
| `hardware/rtl/mldsa/` | 29 | NTT, rounding, hints, sampling, and whole KeyGen / Sign / Verify cores behind the shared `mldsa_engine` (44/65/87 selected at run time) |
| `hardware/rtl/keccak/` | 2 | `keccak_f1600` permutation and the `sha3_core` sponge |
| `hardware/rtl/sym/` | 6 | AES-128/256, SM4, SM3 |
| `hardware/rtl/trng/` | 6 | Ring-oscillator source, RCT/APT health tests, SHA-3 conditioning, erasable FIFO, AXI wrapper |
| `hardware/rtl/bus/` | 9 | AXI4-Lite firewall, symmetric key registers (`key_vault`, `key_vault_axi`, `sym_vault_top`), per-core AXI wrappers |
| `hardware/rtl/common/` | 2 | Dual-port RAM, synchronous FIFO |
| `hardware/rtl/board/` | 2 | `axi4lite_xbar` crossbar and the `zu3eg_hsm_top` board top level |
| `hardware/platform/fan_ctrl/` | 4 | Fan control. Not crypto; kept in a separate tree and shares only clock and reset |

Algorithm inventory, parameter sets and validation status are in
[reference/security-policy.md](reference/security-policy.md).

ML-KEM uses one `sha3_core` for all four Keccak roles (G, PRF, XOF, H). `mldsa_axi`
has its own `sha3_core`, so a zeroize or tamper on one slave does not touch the
other's sponge state. Decaps instantiates the Encaps core for its re-encryption
step, as FIPS 203 §7.3 defines the operation.

Constant time matters in one place in the PL: the Decaps implicit-reject comparison
compares every byte and then selects, with no early return.

## Symmetric key registers and firewall

`key_vault_axi` is a set of 8 symmetric key registers. It holds a key for one
session. It is not a store: nothing in the PL persists across power-off, and the
long-term copy of a key is a KEK-wrapped blob outside the PL.

- Load. The TA writes a key through the PTA and SiP `0x8200ff16` as a sequence of
  `SLOT_SEL`, `BEGIN`, eight `KEY_IN` words, `COMMIT`. The three load registers
  accept `AxPROT[1]==0` only, independent of `SECURE_ONLY`. A normal-world write
  there is dropped with an OKAY response and counted in `LOAD_REJ`. A slot is
  usable only after `COMMIT`, so a partial load cannot be used.
- Use. The key reaches the symmetric cores only over the `use_key` wires. The RTL
  has no bus-side read of the key registers; `KEY_IN` reads 0 and every other
  register is metadata. `test_key_vault` scans the 256-byte address space word by
  word and asserts that no written key word appears. The daemon cannot retrieve a
  key either.
- Wipe. `CTRL.ZEROIZE` (also reachable from `sym_axi`'s `CTRL.ZEROIZE` through
  `ext_zeroize`) clears all registers in one clock, so no half-erased state exists.
  The daemon issues it when a session ends. A `tamper` input clears them too and is
  one-way, never un-erased.
- Violations are counted (`VIOL_CNT`, `VIOL_INFO`, `LOAD_REJ`), readable from the
  secure world, so a refused access can be told from one that never happened.
- The `tamper` input is driven by the SYSMONE4 over-temperature output after it has
  been high for 2^16 consecutive cycles (about 0.87 ms) and the ADC has produced a
  result. Nothing else is connected to it, and the over-temperature path has not
  been exercised on the board.

`axi4lite_firewall` gates each slave. With `SECURE_ONLY=1` it requires
`AxPROT[1] == 0`; anything else reads back 0 and has its write discarded, with no
bus error (RAZ/WI). Two details matter:

- Refused reads never pop the FIFO. Otherwise a non-secure read that gets no data
  could still consume random words and drain the entropy pool. With RAZ/WI the
  response code no longer tells allowed from refused, so "the FIFO did not move" is
  what shows the transaction went nowhere.
- Refusal is RAZ/WI, not DECERR. A refused write is posted, so a DECERR comes back
  as an SError and the kernel can only panic, which costs a power cycle. The board
  took that once from the kernel's own GPIO probe. Refusal stays visible: every
  `VERSION` is a nonzero constant, so reading 0 is the signal, and the violation
  counters keep a record. What is given up is the diagnosability of a mistyped
  address, in exchange for no user-space program being able to take the board down.

The firewall stops the normal world from issuing its own transactions. It does not
stop root from going through the generic EL3 register SiP. That is why the BL31
whitelist excludes the whole of slot 1 and the seed ports; the two halves work
together.

## Entropy source

```
ring-osc array ──sample/decimate──┬──▶ continuous health tests (RCT + APT)
                                  └──▶ Keccak sponge conditioning ──▶ FIFO ──▶ AXI
```

Eight ring oscillators of different lengths (13/15/.../27 stages), two-stage
synchronisers, then decimation.

- Conditioning is a Keccak sponge. SP 800-90B §3.1.5.1.2 lists vetted conditioning
  constructions, so output entropy can be counted as `min(output length, input
  entropy)`. It reuses `keccak_f1600` at no extra area.
- The health tests consume the post-decimation stream, the same stream the
  conditioner uses.
- An alarm clears the pool as well: `ready` goes low, the FIFO is erased, the
  conditioner and sponge are reset and the startup tests run again. The standard
  only requires stopping output; an alarm means the source may have been degraded
  for some time, so the pool is discarded.
- Cutoffs come from the measured entropy. See [TESTING.md](TESTING.md#entropy).

The TA draws key entropy from the OP-TEE PRNG mixed with words from this TRNG
(SiP `0x8200ff17`), and the SiP returns an error if the TRNG health check alarms.

## Key management

Keys belong to the TA. This table is the authoritative summary of where each kind
of key lives.

| Key | At rest | While in use |
|---|---|---|
| AES / SM4 | KEK-wrapped blob (`PWRP`, AES-256-GCM) in a normal-world keystore file | The TA unwraps it and loads it into a key register slot through PTA and SiP `0x8200ff16`. The slot holds it until the session ends, then it is wiped |
| ML-KEM `dk`, ML-DSA `sk` | The TA-wrapped seed blob plus the public key, in a keystore file. No private key is stored anywhere | For each operation the TA unwraps the seed and writes it to the PL staging port (SiP `0x8200ff15`). The PL expands it (`MODE[4]=CHAIN`), computes, and wipes the expansion region itself. Software waits for `KEYSTAT[0]` (`EXP_WIPING`) to clear before the next operation |
| KEK | Not stored. Derived inside the TA from the HUK | Never leaves the TA |

The daemon `pqchsm_fpgad` only passes blobs between the application and the TA. It
never sees a plaintext key and cannot unwrap a blob. Because the KEK is bound to
the board, a keystore file copied to another board cannot be unwrapped.

The KeyGen path that takes `d‖z` (ML-KEM) or `ξ` (ML-DSA) from software exists for
ACVP and still returns the private key through `OUT_DATA`. `CTRL.SEED_LOCK`
restricts KeyGen to the seed port; it is a latch that only a power cycle clears,
and the daemon does not set it.

What this does not give: the HUK is `SHA-256(Device DNA)` and no eFUSE is burned
(see [Key hierarchy](#key-hierarchy)), so the KEK is bound to the chip but not
secret from someone who can read the DNA. The normal-world kernel can still have
the TA load a key from a blob it holds, and use keys that are already loaded.

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
`pqc_backend_t` vtable works on opaque handles. No function declared in
`include/pqchsm/` returns private key material: key generation returns a handle,
signing takes a handle and a message, decapsulation takes a handle and a
ciphertext. A handle encodes the slot and the slot's generation counter, so
destroying an object invalidates every handle issued for it.

`accel.h` fixes the register contract (`CTRL`, `STATUS`, `MODE`, `PARAM`, `IN_LEN`,
`OUT_LEN`, `ERRCODE`). Four transports implement it: a software stub,
Verilator-simulated RTL, the same RTL driven over AXI4-Lite and AXI4-Stream, and
`/dev/mem` + `mmap` on the board. The stub and the simulated RTL must give
byte-identical results, which `tests/unit/test_accel.c` asserts. `accel_shake()`
implements the sponge in C and calls the transport only for the permutation.

The path that runs on the board is `service/`: application, `libsdfe`,
`pqchsm_fpgad`, `/dev/secmmio`, BL31, PL, with the TA beside it for keys. The
ACVP results, the symmetric vectors, the entropy capture and the boundary
measurements come from the standalone programs in `board/src/` and from
`service/`. The `src/` stack above `accel_transport_t` (PKCS#11, slot manager,
keystore) has not been driven against the real PL.

## Key hierarchy

On the board:

```
Device DNA  (read-only, not secret)
 └── HUK = SHA-256(Device DNA)        OP-TEE fork for ZynqMP, no eFUSE
      └── TA-unique key               PTA_SYSTEM_DERIVE_TA_UNIQUE_KEY, extra data "pqchsm-kdr-v1"
           └── KEK = KMAC256(K = TA-unique key, X = salt, S = "pqc-hsm/storage-kek")
                └── PWRP blob = AES-256-GCM(KEK, key material)
```

The salt is a fixed per-device constant held by the daemon and sent to the TA at
startup (`CMD_KEK_SET`). It is not a secret; changing it makes existing blobs
unopenable. The KEK is cached inside the TA and has no path out. A blob is
`"PWRP" ‖ version ‖ alg ‖ lengths ‖ nonce ‖ ciphertext ‖ tag` (`tee/ta/ta_wrap.h`).

Device DNA can be read through JTAG, so someone holding the DNA and a blob can
derive the KEK. The binding prevents moving a keystore to another board; it does
not protect against an attacker who has the board's DNA. A HUK that is secret
depends on authenticated boot with eFUSE, which this project does not burn.

The host stack in `src/` (PKCS#11 front end, keystore, backup) has its own hierarchy:

```
KDR  (32 bytes; DEV builds: fixed stub; PQCHSM_KDR=device-dna: bound to Device DNA)
 └── KEK        wraps key material in the keystore file
RMK  (recovery master key, randomly generated)
 └── BEK        wraps backup blobs; portable to a replacement device
      └── Shamir M-of-N shares over GF(256)
```

The two wrapping keys have different purposes: material under a device-bound key is
safe but unrecoverable if the device dies; material under a portable key is
recoverable but only as well protected as its shares. Each slot carries a policy bit
saying whether its key takes part in backup. A keystore file is per-slot blobs plus
a whole-file MAC:

```
slot blob := plaintext metadata ‖ AES-256-GCM(KEK, aad = metadata, key material ‖ PIN material)
file      := header ‖ slot blob × N ‖ KMAC256(file)
```

Metadata is authenticated but not encrypted. Writes are atomic (temporary file,
`fsync`, `rename`, `fsync(dir)`). Every state transition appends an audit record
whose SHA3-256 hash covers the previous record's hash; the head is signed with an
ML-DSA device identity key.

`hsmctl backup` is a different, simpler mechanism: it packs the TA-wrapped blobs
with an SM3 digest per entry, and restores only on the same board.

Key injection provisions a key without exposing it: the provisioning host
encapsulates to the device's ML-KEM public key and wraps a seed under the resulting
shared secret. FIPS 203 and 204 keys are determined by their seed, so this moves
fewer sensitive bytes and lets the device verify the expansion itself.

## Integration notes

`hardware/rtl/board/zu3eg_hsm_top.v` wires everything together. Three things in it
were found on real hardware:

`M_AXI_HPM0_LPD` is AXI4, not AXI4-Lite. Treating it as Lite leaves `bid`, `rid` and
`rlast`, all driven by the PL and returned to the PS, dangling. The PS waits for
`rlast` forever and the CPU hangs on that load. Synthesis, place-and-route, timing
and bitstream generation are all clean, and the board reports "operating".
AXI4-Lite is AXI4 with burst length fixed at 1, so `rlast = rvalid` and `bid`/`rid`
echo `awid`/`arid`. The implementation flow has a post-synthesis assertion for it.

Declaration order matters. Referencing a `wire` declared later does not error in
Vivado; it silently creates a new, undriven net of the same name at that port.
Icarus rejects the same code, which is why the lint flow has vendor stubs: excluding
the board top level from lint would remove the only automatic check for this.

Fan safety errs toward more airflow. Minimum duty is 25 %; 80 °C and above forces
100 % and holds until 74 °C; a stale temperature forces 100 %; and a reading that
never changes a single bit also forces 100 %. The last rule was added after a board
run in which the first SYSMON configuration left the ADC not converting while DRP
still answered with a plausible 32.5 °C. A real junction temperature always
jitters, so perfect stability is itself the fault signal.
