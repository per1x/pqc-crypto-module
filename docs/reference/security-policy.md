**English** · [中文](security-policy.zh-CN.md)

# Security policy (draft)

This is a draft written against a research prototype. It follows the structure of a
FIPS 140-3 (ISO/IEC 19790:2012) non-proprietary security policy and maps onto the
corresponding clauses of GM/T 0028-2014, so that the gap between what exists and
what a submission would require is explicit. Section 13 lists the gaps. No
validation has been sought or obtained, and several sections describe requirements
the module does not currently meet.

It describes the current form: keys are managed by an OP-TEE trusted application
(TA) in the secure world, and the PL computes and holds no long-term key.

## 1. General

| Item | Value |
|---|---|
| Module name | pqc-crypto-module |
| Module type | Hybrid: the cryptographic engine is the programmable logic (PL) of a Xilinx XCZU3EG; key management runs in an OP-TEE TA on the Cortex-A53; host software is the control plane outside the boundary |
| Target security level | The hardware boundary exists and is enforced. A level-3 claim would additionally require physical tamper response, a secret key root, and algorithm certificates (see §13) |
| Embodiment | FPGA bitstream on an XCZU3EG (AXU3EGB board); Cortex-A53 running Linux (normal world), BL31 (EL3) and OP-TEE with the TA (secure world); host side is a shared library (`pqchsm-pkcs11`), the board daemon `pqchsm_fpgad`, `libsdfe`, `hsmctl` and `hsm_httpd` |
| Cryptographic boundary (as drawn here) | The PL: ML-KEM 512/768/1024, ML-DSA 44/65/87, AES-128/256, SM4, SM3, the ring-oscillator TRNG, the symmetric key registers (`key_vault_axi`), the per-operation expansion regions of `mlkem_axi` and `mldsa_axi`, and the AxPROT-gated AXI firewall around them. Key generation and wrapping run in the TA, outside this PL boundary; the enforcement evidence below covers the PL only |
| Boundary enforcement | `axi4lite_firewall` refuses any transaction with `AxPROT[1]=1` to a `SECURE_ONLY=1` slave: the read returns 0, the write is discarded, no bus error is raised (RAZ/WI). Shown in both directions on the board; see the evidence below |
| Tested operational environment | XCZU3EG (`xazu3eg-sfvc784-1-i`) at 75 MHz, Linux 5.4 on the Cortex-A53; host tooling on macOS arm64 and Debian aarch64 (GCC 12) |

**Boundary enforcement evidence.** Measured on the board, one run, one bitstream,
one address each:

| | `0x8004_0000` (`SECURE_ONLY=1`) | `0x8003_0000` (`SECURE_ONLY=0`) |
|---|---|---|
| Secure world, EL3 (`AxPROT[1]=0`) | reads `0x00010000` | reads `0x00010000` |
| Normal world, EL1-NS (`AxPROT[1]=1`) | refused: reads back `0`, write discarded, no bus error | reads `0x00010000` |

The right-hand column is the control: the same normal world reads a core without the
gate, so the refusal on the left comes from the gate and not from an unreachable
address.

This table is from an earlier bitstream in which slot 3 (`mlkem_axi`) was
`SECURE_ONLY=0`. In the current default bitstream all functional slaves are
`SECURE_ONLY=1` (set in one place, `SECURE_ONLY_FUNCTIONAL` in `zu3eg_hsm_top.v`),
so that column no longer exists there. To repeat the comparison, use the
development bitstream (`PQC_DEV_OPEN=1`, functional slaves `SECURE_ONLY=0`) for the
right column and the slot 4 canary for the left. On the default bitstream the left
column was checked on 2026-08-17: normal-world reads of slots 0, 1, 2, 3 and 6 all
returned 0 (`board/logs/RESULT_secform_mldsa.txt`). The secure-world side is a
minimal EL3 payload: one SiP call added to BL31 that performs one read and returns
the value. Raw logs and the method are in [SECURITY.md](../SECURITY.md).

A separate measurement covers the symmetric key registers: scanning 256 bytes of
each of two instances (48 readable, 80 refused by the firewall) found none of the
key's four words anywhere, while the ciphertext those keys produced was correct. A
third measurement wrote the load port from the normal world on the development
bitstream: all 11 writes were dropped, `LOAD_REJ` went from 0 to 11, and the entry
stayed empty.

The module provides post-quantum key encapsulation and digital signature services,
symmetric and SM-series algorithms, key storage as wrapped blobs, slot and session
management, backup and recovery, and a tamper-evident audit log, through a PKCS#11
v3.2 interface (host stack), an SDF-style interface, a command line and an HTTP
gateway (board).

## 2. Cryptographic module specification

The host stack is defined by the sources under `src/` and `include/pqchsm/`, built
into `libpqchsm` and the PKCS#11 shared library. It links OpenSSL 3. The
post-quantum algorithms come from the bundled mlkem-native / mldsa-native
(`third_party/pqc-native/`), which the OP-TEE TA also uses. These libraries and
OpenSSL are outside the PL boundary, and their algorithm implementations are used
unmodified.

The algorithm cores and their parameter sets are listed in
[ARCHITECTURE.md](../ARCHITECTURE.md#cryptographic-cores); validation status is in
[TESTING.md](../TESTING.md).

The module has one mode of operation. There is no non-approved mode: services that
would require a non-approved algorithm are not offered.

An accelerator abstraction (`include/pqchsm/accel.h`) allows the arithmetic to be
executed by programmable logic instead of software. Four transports implement it
identically: a software stub, a simulated RTL backend, the same RTL driven over
AXI4-Lite and AXI4-Stream, and a memory-mapped transport for a real device. The
register contract they share is in [REGISTERS.md](../REGISTERS.md).

On the tested operational environments the host stack (`src/`) uses the software
stub. The service path (`service/`, daemon, `/dev/secmmio`, EL3, PL) runs on real
hardware, and ML-KEM, ML-DSA, AES, SM4, SM3 and the TRNG have been checked against
official vectors on the board (see [TESTING.md](../TESTING.md) and `board/logs/`).
Both statements are true at the same time; the coverage tables are in
[API.md](../API.md).

## 3. Cryptographic module interfaces

| Logical interface | Realisation |
|---|---|
| Data input | PKCS#11 function arguments; TLV command protocol (`src/proto`); SDF-style calls (`libsdfe`); `hsmctl` files; HTTP requests |
| Data output | PKCS#11 return buffers; TLV responses; SDF return values; HTTP responses |
| Control input | PKCS#11 function selection; CLI and admin tool commands; daemon wire operations |
| Status output | PKCS#11 return codes; `hsm_status_t`; the audit log; `HSM_STATUS` on the board |
| Power interface | Not applicable to the software parts |

No function in `include/pqchsm/` and no application interface (`libsdfe`, `hsmctl`,
HTTP) returns private key material. Every API in `include/pqchsm/` operates on
opaque handles; `tools/check_no_readback.py` enforces this structurally by failing
if a read-back function is added to the key derivation root header. The one
exception is at the PL register level: KeyGen with a software-supplied seed (the
ACVP path) returns the private key through `OUT_DATA`. See §13.

## 4. Roles, services, and authentication

Three roles are defined in the host stack (`hsm_role_t`):

| Role | Authentication | Services |
|---|---|---|
| Public | None | Read public token information, read public keys, verify signatures |
| User | User PIN | Key generation, signing, encapsulation, decapsulation, object management within the slot |
| Security officer | SO PIN | Token initialisation, PIN reset and unlock, zeroisation, backup and recovery |

Authentication is PIN-based. PINs are stored as a KMAC256 digest with a per-slot
salt; comparison uses the constant-time primitive. User PIN failures increment a
counter and lock the slot at the configured limit; only the security officer can
unlock it.

Roles are enforced by an explicit access-control table checked on every service
entry, and the slot state machine (`src/slot/fsm.c`) is an explicit transition
table. Both are exercised directly by `tests/unit/test_slot_fsm.c`.

The board service path (`pqchsm_fpgad`, `hsmctl`, `hsm_httpd`) has no roles or PINs.
The local UNIX socket (mode 0600) and the loopback HTTP port (no authentication) trust the processes that can reach them on the board; the
remote port and `:8443` require a client certificate. A process that can open a TA
session as `LOGIN_USER` can have the TA load a key from a blob it holds. Per-caller
authorisation is not implemented.

## 5. Software security

The module is delivered as source and built by the integrator. Integrity of the
built artefact is the responsibility of the operational environment; the module
performs no signature check on itself. A validation submission would require an
approved integrity technique over the module image, which does not exist here. On
the board, the boot chain (BOOT.BIN, BL31, OP-TEE, the TA) is not authenticated: no
eFUSE is burned, and RSA authenticated boot without eFUSE is not a root of trust.

The keystore file of the host stack carries a whole-file KMAC256 tag, so record
deletion, reordering or truncation is detected on load. The audit log is a
SHA3-256 hash chain whose head is signed with an ML-DSA device identity key.

## 6. Operational environment

This section is about the host software stack, not the PL boundary. The two are
different things:

* The cryptographic boundary is the PL cores plus the firewall. It is a fixed
  hardware configuration; changing it means a new bitstream.
* The host software stack (`src/`, the PKCS#11 module) is outside the boundary. It
  is modifiable, runs on a general-purpose operating system, and corresponds to
  security level 1 under FIPS 140-3, with process separation left to the operating
  system.
* The daemon (`service/`) is outside the boundary too, and is the only driver of the
  PL from the normal world. The TA runs in the secure world under OP-TEE and its
  integrity depends on the boot chain.

What level can be claimed therefore depends on which part is meant. The boundary
has hardware-gate evidence (§1), but a whole-module level-3 claim lacks physical
tamper response and a secret key root (§13).

Sensitive buffers are `mlock`-ed where the platform permits and zeroised after use.
Neither defends against an attacker who can read the process address space; see
[constant-time.md](constant-time.md) for what the audits cover.

## 7. Physical security

No physical security exists. The PL has a `tamper` input that clears the symmetric
key registers and the other cores when asserted. It is driven by the SYSMONE4
over-temperature output after it has stayed high for 2^16 cycles, and that path has
not been triggered on the board. There is no tamper-evident enclosure, tamper
response or environmental failure protection. A standalone embodiment on a Zynq-class
SoC would need them.

## 8. Non-invasive security

Not claimed. The constant-time audit (`tools/ct_audit.py`) verifies that no
secret-dependent branch, memory index or divisor exists in the module's own sources
under `src/`, and `tests/unit/test_ct_timing.c` verifies statistically that the
constant-time comparison primitive is data-independent. Neither is a non-invasive
attack mitigation claim in the sense of ISO/IEC 19790 Annex F: OpenSSL, the bundled
mlkem-native / mldsa-native and the RTL are outside the audit, and no power or
electromagnetic analysis has been performed.

## 9. Sensitive security parameter management

| Parameter | Generated | Stored | Zeroised |
|---|---|---|---|
| Symmetric keys (AES, SM4) | In the TA (OP-TEE PRNG mixed with PL TRNG words) | Outside the TA only as `PWRP` blobs (AES-256-GCM under the KEK) | In the PL key register slot until the session ends, then cleared by `ZEROIZE` (one clock); also on `tamper` |
| PQC private-key seeds (ML-KEM `d‖z`, ML-DSA `ξ`) | In the TA | As `PWRP` blobs with the public key. The PL stores no seed | The staged seed is cleared by the hardware after one use; the expansion region is wiped by the hardware at the end of each operation (`KEYSTAT[0]`) |
| KEK | Derived in the TA: HUK, then the TA-unique key, then `KMAC256` with a per-device salt | Not stored; cached in TA memory | TA instance teardown |
| HUK | `SHA-256(Device DNA)`, no eFUSE | Not stored | Not applicable. Device DNA is readable |
| Host stack: KDR, KEK, RMK, BEK, PIN keys | See [ARCHITECTURE.md](../ARCHITECTURE.md#key-hierarchy) | Keystore file (wrapped), RMK only as Shamir shares | `tools/check_zeroize.py` checks every field |

The key hierarchy is in [ARCHITECTURE.md](../ARCHITECTURE.md#key-hierarchy).

**Entropy.** The boundary contains a ring-oscillator noise source with SP 800-90B
continuous health tests (`hardware/rtl/trng/`). The TA draws key entropy from the
OP-TEE PRNG mixed with TRNG words (SiP `0x8200ff17`, which returns an error if the
health check alarms). In the host stack, `pqc_random_bytes()` prefers the hardware
source and fails rather than silently falling back (`src/util/util.c`); all seven
kinds of key material there (private-key seeds, PIN keys and salts, the backup RMK,
Shamir coefficients, the KEK salt, the wrap nonce and the KEM-DEM IV) go through it.
Only on a host with no transport installed (a pure-software build, or the unit
tests) does it fall back to the OpenSSL DRBG seeded by the operating system, and the
caller can see this through `hwrng_available()`. The measured min-entropy of the raw
noise is 0.871234 bits per sample; the restart matrix is in
`board/logs/RESULT_restart.txt`.

Zeroisation of host memory is structural: `tools/check_zeroize.py` verifies that
every key-material field is wiped by its destructor and that every local key buffer
is wiped on every return path, and fails the build otherwise.
`tests/unit/test_zeroize.c` additionally verifies at `-O2` that the zeroising
primitive is not removed as a dead store, with a negative control that confirms a
plain `memset` in the same position is removed. In the TA, an unwrapped key is
cleared with `pqchsm_bzero` after use.

Inside the PL, zeroisation takes two forms:

- The symmetric key registers are a register array: `zeroize` / `tamper` clears
  every slot on one clock edge, so no half-erased window exists. That is one reason
  they are registers and not BRAM. The daemon sends `ZEROIZE` when a session ends.
- The ML-KEM buffers and the per-operation expansion regions of `mlkem_axi` and
  `mldsa_axi` are BRAM and can only be cleared address by address (8192 cycles), so
  a half-erased window exists. The design exposes it as a status bit (`WIPING`, and
  `KEYSTAT[0]` for the expansion region). While it is set, reads and writes are
  refused and the core refuses to start, answering SLVERR rather than silently
  discarding writes, because a silent discard would let software start with the
  wrong input length and get a quietly wrong result. The criterion in simulation is
  a read-back of all 16384 bytes of both BRAMs confirming zero, not `OUT_LEN`
  becoming 0 (that only shows software can no longer read it).

  This cannot be shown on the board, because software has no path to those bytes in
  the first place. What the board shows is the behaviour and duration of `WIPING`.

The PS-side XPPU and FPD_XMPU do not cover the PL window at `0x8000_0000` (UG1085
v2.5 Table 16-10 lists every aperture; `M_AXI_HPM0_LPD` bypasses the FPD per
p1092). The AxPROT-gated firewall in the PL is therefore the only enforcement point
on this route, which is why the bus decode must be one-to-one with no mirrored
addresses.

## 10. Self-tests

Pre-operational self-tests of the host stack run before the first cryptographic
service and can be re-run explicitly (`pqc_self_test()`). Each is a known-answer
test whose expected value comes from outside the module:

| Test | Source of the expected value |
|---|---|
| SHA3-256 | FIPS 202 published example |
| KMAC256 | SP 800-185 official sample #4 |
| AES-256-GCM | GCM specification test case 14 |
| ML-KEM-768 key generation | NIST ACVP ML-KEM-keyGen-FIPS203 vector |
| ML-DSA-65 key generation | NIST ACVP ML-DSA-keyGen-FIPS204 vector |

Any failure places the module in an error state in which every cryptographic
service returns `PQC_ERR_SELF_TEST` and no output is produced. Recovery requires a
successful re-run. `tests/unit/test_selftest.c` verifies that the error state blocks
every entry point, including when the arguments are themselves invalid.

The board daemon does not run these at start-up. On the board the 24-item
self-test (`hsm_hwtest`, results in `board/logs/RESULT_hwtest.txt`) and the ACVP
runs are started by hand.

Conditional self-tests required by FIPS 140-3 that are not implemented: pairwise
consistency on freshly generated key pairs, and a continuous random number
generator test. Both are listed in §13.

## 11. Life-cycle assurance

Development is version-controlled; every change carries its tests. The test suite
runs under `ctest` on macOS/arm64 and Debian/aarch64, under AddressSanitizer and
UndefinedBehaviorSanitizer, and under ThreadSanitizer for the concurrent paths.
Correctness of the algorithm path is pinned to NIST ACVP vectors;
`tools/kat_evidence.sh` regenerates the evidence table by running them.

The RTL is checked by a cocotb regression of 272 tests against independent oracles
and by `tools/rtl_lint.sh` (Verilator `-Wall`, every module as its own top level,
plus an Icarus Verilog pass). `rtl_lint.sh` currently fails: seven modules
(`mldsa_axi`, `mldsa_engine`, `mldsa_keygen`, `mldsa_sign`, `mldsa_verify`,
`mlkem_axi`, `zu3eg_hsm_top`) still have Verilator warnings. Waivers are listed
with reasons in `hardware/rtl/lint_waivers.vlt` and matched per file and signal, so
a new occurrence of the same class is not silently covered.

Delivery is source-only. There is no signed release artefact, no formal
configuration-management record, and no operator guidance document beyond the
repository documentation.

## 12. Mitigation of other attacks

None claimed beyond the constant-time properties described in §8 and in
[constant-time.md](constant-time.md).

## 13. Gaps relative to a validation submission

Each item below would independently block a submission. They are listed so the
document is not read as a statement of readiness.

1. **The key root is bound to the chip but not secret.** On the board the HUK is
   `SHA-256(Device DNA)` and no eFUSE is burned. Device DNA can be read through
   JTAG, so someone holding the DNA and a blob can unwrap it. In the host stack the
   DEV-build KDR is a compiled-in constant (a PRODUCTION build fails without a
   provided root); `PQCHSM_KDR=device-dna` binds it to the DNA, which prevents
   cloning but gives no secrecy. A secret root needs authenticated boot with eFUSE.
2. **Entropy source inside the boundary: closed.** A ring-oscillator noise source
   with SP 800-90B continuous health tests runs inside the boundary; measured
   min-entropy is 0.871234 bits/sample. The host stack still seeds from OpenSSL when
   no transport is installed.
3. **No module integrity check.** Nothing verifies the module image before use.
4. **Conditional self-tests missing.** No pairwise consistency test on generated
   key pairs, no continuous RNG test.
5. **No physical security.** The boundary is enforced logically (the AxPROT gate).
   There is no tamper-evident enclosure, no tamper response, no environmental
   failure protection. The `tamper` input is wired only to the SYSMONE4
   over-temperature output (see §7), and that path has not been exercised.
6. **No algorithm certificates.** ACVP vectors are run locally; that is evidence of
   correctness, not a certificate.
7. **Single-writer audit log.** The audit module does not lock the file.
8. **Unkeyed Shamir share checksums.** They detect corruption, not tampering.
9. **SO PIN lockout not enforced.** Failures are counted but the slot is not locked,
   because locking it would brick the device; recovery is by M-of-N.
10. **Whole-core ML-DSA in hardware: closed.** ML-DSA 44/65/87 KeyGen/Sign/Verify
    have whole cores behind `mldsa_axi` (slot 6) and match the NIST ACVP vectors
    byte for byte on the board (32/32; see `board/logs/` and
    [TESTING.md](../TESTING.md)). ML-KEM 512/768/1024 and the symmetric cores
    (AES-128/256, SM4, SM3) likewise match their standard vectors on the board.
11. **The ACVP KeyGen path outputs the private key.** KeyGen with a software-supplied
    `d‖z` or `ξ` returns the private key through `OUT_DATA`. `CTRL.SEED_LOCK`
    restricts KeyGen to the seed port, but it is a latch cleared only by a power
    cycle, and the daemon does not set it.
12. **Root can still make the hardware compute.** The daemon issues operation
    sequences through the EL3 whitelist, and the SiP does not check the caller.
    Key loading is secure-world only, so the normal world cannot read or inject
    keys, but it can use keys that are already loaded. The DDR-side XMPU only
    prevents reads (it poisons transactions and the endpoint refuses them).
13. **No authenticated or encrypted boot.** BBRAM is blocked by the board (no
    battery on `VCC_BATT`) and the PUF black key is rejected by the BootROM. Preventing
    image replacement requires burning eFUSE, and this project does no irreversible
    programming. See [SECURITY.md](../SECURITY.md).

## Mapping to GM/T 0028-2014

GM/T 0028-2014 is aligned with ISO/IEC 19790:2012, so the section structure
corresponds closely. The table records where each requirement is addressed in this
document.

| GM/T 0028 clause | Subject | This document |
|---|---|---|
| 7.1 | 密码模块规格 | §1, §2 |
| 7.2 | 密码模块接口 | §3 |
| 7.3 | 角色、服务和鉴别 | §4 |
| 7.4 | 软件/固件安全 | §5 |
| 7.5 | 运行环境 | §6 |
| 7.6 | 物理安全 | §7 |
| 7.7 | 非入侵式安全 | §8 |
| 7.8 | 敏感安全参数管理 | §9 |
| 7.9 | 自测试 | §10 |
| 7.10 | 生命周期保障 | §11 |
| 7.11 | 对其他攻击的缓解 | §12 |

Two differences matter for a GM/T 0028 submission and are not addressed by this
module. The standard's approved algorithm list is the Chinese commercial
cryptography suite (SM2, SM3, SM4, ZUC). SM4 and SM3 are in the PL and have been
checked on the board against GB/T 32907 A.1 and GB/T 32905 A.1. SM2 and ZUC have
neither a core nor a software implementation; SM2 is the largest gap for GM/T
certification, and the gap list is in [API.md](../API.md). The second difference is
that the randomness requirements reference GM/T 0005 rather than SP 800-90B, so the
noise source health tests in `hardware/rtl/trng/` would need to be re-derived
against that document's thresholds.
