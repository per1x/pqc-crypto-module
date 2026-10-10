**English** · [中文](security-policy.zh-CN.md)

# Security policy (draft)

> **⚠️ Form note (batch 1, in progress).** Parts of the implementation this
> document describes have moved: the ML-KEM/ML-DSA key seeds are now generated
> and held in EL3 rather than in the normal-world daemon, and the PL cores have
> a real erase machine. **None of it is verified on the board yet.** The body
> below is deliberately left describing the *previous* form — rewriting a
> submission-shaped document ahead of the evidence would promise a capability
> that has not been delivered. See
> [FINAL-PLAN.zh-CN.md](FINAL-PLAN.zh-CN.md) §8 for what changed and what is
> still pending on silicon.


> **This is a draft written against a research prototype.** It follows the
> structure a FIPS 140-3 (ISO/IEC 19790:2012) non-proprietary security policy
> takes, and maps onto the corresponding clauses of GM/T 0028-2014, so that the
> gap between what exists and what a submission would require is explicit rather
> than implied. Section 13 lists the gaps. No validation has been sought or
> obtained, and several sections describe requirements the module does not
> currently meet.

## 1. General

| Item | Value |
|---|---|
| Module name | pqc-crypto-module |
| Module type | Single-chip hardware module: the cryptographic engine is the programmable logic of a Xilinx XCZU3EG, with host software as the control plane outside the boundary |
| Target security level | The hardware boundary exists and is enforced; a level-3 claim would additionally require physical tamper response, a device-bound key derivation root, and algorithm certificates — see §10 |
| Embodiment | FPGA bitstream on an XCZU3EG (AXU3EGB board), driven over AXI4 from a Cortex-A53 running Linux; host side is a shared library (`pqchsm-pkcs11`) plus a daemon and CLI |
| Cryptographic boundary | The programmable logic: ML-KEM 512/768/1024, AES-128/256, SM4, SM3, the ring-oscillator TRNG, the key vault, and the AxPROT-gated AXI firewall that encloses them |
| Boundary enforcement | `axi4lite_firewall` refuses any transaction with `AxPROT[1]=1` to a `SECURE_ONLY=1` slave: the read returns 0, the write is discarded, no bus error is raised (RAZ/WI). Proven in both directions on silicon — see the evidence below |
| Tested operational environment | XCZU3EG (`xazu3eg-sfvc784-1-i`) at 75 MHz, Linux 5.4 on the Cortex-A53; host tooling on macOS arm64 and Debian aarch64 (GCC 12) |

**Boundary enforcement evidence.** Measured on the board, one run, one bitstream,
one address:

| | `0x8004_0000` (`SECURE_ONLY=1`) | `0x8003_0000` (`SECURE_ONLY=0`) |
|---|---|---|
| Secure world, EL3 (`AxPROT[1]=0`) | reads `0x00010000` | reads `0x00010000` |
| Normal world, EL1-NS (`AxPROT[1]=1`) | **refused** (logged as DECERR/SIGBUS; the current RAZ/WI bitstream refuses by reading back `0`) | reads `0x00010000` |

The right-hand column is the control: the same normal world reads a non-gated core
successfully, so the refusal on the left is the gate and not an unreachable address.
The secure-world side is a minimal EL3 payload — a single SiP call added to BL31 that
performs one read and returns the value. Raw logs and the full method are in
[SECURITY.md](../SECURITY.md)

A separate measurement covers the key vault: scanning 256 bytes of each of two slaves
(48 readable, 80 refused by the firewall) found none of the key's four words anywhere,
while the ciphertext those keys produced was correct.

The module provides post-quantum key encapsulation and digital signature
services, key storage, slot and session management, M-of-N backup and recovery,
and a tamper-evident audit log, exposed through a PKCS#11 v3.2 interface.

## 2. Cryptographic module specification

The module is defined by the sources under `src/` and `include/pqchsm/`, built
into `libpqchsm` and the PKCS#11 shared library. It links OpenSSL 3 and liboqs;
both are outside the boundary as drawn here, and their algorithm
implementations are used unmodified.

The approved algorithms, their parameter sets, and the evidence supporting each
are listed in [ARCHITECTURE.md](../ARCHITECTURE.md#cryptographic-cores).

The module has one mode of operation. There is no non-approved mode: services
that would require a non-approved algorithm are not offered.

An accelerator abstraction (`include/pqchsm/accel.h`) allows the arithmetic to
be executed by programmable logic instead of software. Four transports implement
it identically — a software stub, a simulated RTL backend, the same RTL driven
over AXI4-Lite and AXI4-Stream, and a memory-mapped transport for a real
device — and the register contract they share is documented in
[REGISTERS.md](../REGISTERS.md).

> **Of these four transports, the host software stack (`src/`) uses the software
> stub on the tested operational environments. Do not read that as "the project is
> software only": the service path (`service/` → daemon → `/dev/secmmio` → EL3 →
> PL) is real hardware**, and ML-KEM, ML-DSA, AES, SM4, SM3 and the TRNG have all
> been checked against official vectors on the board (see the silicon results in
> [TESTING.md](../TESTING.md) and the raw logs in `board/logs/`). "PKCS#11 runs in
> software" and "the algorithms are verified in hardware" are both true at the
> same time; see the coverage tables in [API.md](../API.md).

## 3. Cryptographic module interfaces

| Logical interface | Realisation |
|---|---|
| Data input | PKCS#11 function arguments; TLV command protocol (`src/proto`) |
| Data output | PKCS#11 return buffers; TLV responses |
| Control input | PKCS#11 function selection; CLI and admin tool commands |
| Status output | PKCS#11 return codes; `hsm_status_t`; the audit log |
| Power interface | Not applicable to a software module |

No interface returns private key material. Every API in `include/pqchsm/`
operates on opaque handles; `tools/check_no_readback.py` enforces this
structurally by failing if a read-back function is added to the key derivation
root header.

## 4. Roles, services, and authentication

Three roles are defined (`hsm_role_t`):

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
entry, not by scattered conditionals, and the slot state machine
(`src/slot/fsm.c`) is an explicit transition table. Both are exercised directly
by `tests/unit/test_slot_fsm.c`.

## 5. Software security

The module is delivered as source and built by the integrator. Integrity of the
built artefact is the responsibility of the operational environment; the module
performs no signature check on itself. A validation submission would require an
approved integrity technique over the module image, which does not exist here.

The keystore file carries a whole-file KMAC256 tag, so record deletion,
reordering, or truncation is detected on load. The audit log is a SHA3-256 hash
chain whose head is signed with an ML-DSA device identity key.

## 6. Operational environment

Modifiable. The module runs as a library inside an arbitrary host process on a
general-purpose operating system. Under FIPS 140-3 this places it at security
level 1 and makes the operating system responsible for process separation.

Sensitive buffers are `mlock`-ed where the platform permits and zeroised after
use. Neither defends against an attacker who can read the process address space;
see [constant-time.md](constant-time.md) for what the audits do and do not
cover.

## 7. Physical security

Not applicable to the software module as it stands. The design anticipates a
multi-chip standalone embodiment on a Zynq-class SoC with the algorithm cores in
programmable logic, at which point physical security requirements (tamper
evidence, tamper response, environmental failure protection) would apply. None
of that exists.

## 8. Non-invasive security

Not claimed. The constant-time audit (`tools/ct_audit.py`) verifies that no
secret-dependent branch, memory index, or divisor exists in the module's own
sources, and `tests/unit/test_ct_timing.c` verifies statistically that the
constant-time comparison primitive is data-independent. Neither is a
non-invasive attack mitigation claim in the sense of ISO/IEC 19790 Annex F: the
algorithm implementations inside liboqs and OpenSSL are outside the analysis,
and no power or electromagnetic analysis has been performed.

## 9. Sensitive security parameter management

The key hierarchy, generation, storage, and zeroisation of every SSP are listed
in [ARCHITECTURE.md](../ARCHITECTURE.md#cryptographic-cores).

**Entropy (corrected 2026-08-17; the previous description was stale).** The
boundary contains a ring-oscillator noise source with SP 800-90B continuous health
tests (`hardware/rtl/trng/`), and it **is** wired into the software path:
`pqc_random_bytes()` prefers the hardware source and **fails rather than silently
falling back** (`src/util/util.c`). All seven pieces of key material — private-key
seeds, PIN keys and salts, the backup RMK, Shamir coefficients, the KEK salt, the
wrap nonce and the KEM-DEM IV — go through it.

Only on a host with no transport installed (a pure-software build, or the unit
tests) does it fall back to the OpenSSL DRBG seeded by the operating system; there
the claim "entropy comes from hardware" does not apply in the first place, and the
caller can see this via `hwrng_available()`. The measured min-entropy of the raw
noise is 0.871234 bits per sample; the restart matrix is in
`board/logs/RESULT_restart.txt`.

Zeroisation is structural rather than incidental: `tools/check_zeroize.py`
verifies that every key-material field is wiped by its destructor and that every
local key buffer is wiped on every return path, and fails the build otherwise.
`tests/unit/test_zeroize.c` additionally verifies at `-O2` that the zeroising
primitive is not removed as a dead store, with a negative control that confirms
a plain `memset` in the same position *is* removed.

Inside the boundary (in programmable logic) zeroisation takes two forms, neither
of which is "reset the pointers":

- The **key vault** is a register array: `zeroize` / `tamper` clears every slot
  **on one clock edge**, so no half-erased window exists. That is one of the
  reasons it is registers and not BRAM.
- **ML-KEM's two 8 KB buffers** are BRAM and can only be cleared address by
  address (8192 cycles), so a half-erased window necessarily exists. The design
  makes that window **an explicit status bit** (`STATUS[4] = WIPING`) and, while
  it is set, refuses every read and write and refuses to start, answering SLVERR
  rather than discarding writes silently — a silent discard would let software
  start with the wrong input length and produce a quietly wrong result. The
  criterion is a simulation **read-back of all 16384 bytes of both BRAMs
  confirming zero**, not "`OUT_LEN` became 0"; the latter only shows software can
  no longer read it, and says nothing about whether the content is still there.

  ⚠️ This one **cannot be shown on the board, and that is the design working**:
  software has no path to those bytes in the first place (if it had, that would
  be the vulnerability). What the board shows is the behaviour and duration of
  `WIPING`.

One point stated precisely: the PS-side XPPU and FPD_XMPU **structurally do not
cover** the PL window at `0x8000_0000` (UG1085 v2.5 Table 16-10 enumerates every
aperture; `M_AXI_HPM0_LPD` bypasses the FPD per p1092). The AxPROT-gated firewall
in programmable logic is therefore not one layer of defence in depth — **it is the
only enforcement point on this routing**, which is precisely why the bus decode
must be one-to-one with no mirrored addresses.

## 10. Self-tests

Pre-operational self-tests run before the first cryptographic service and can be
re-run explicitly (`pqc_self_test()`). Each is a known-answer test whose expected
value comes from outside the module:

| Test | Source of the expected value |
|---|---|
| SHA3-256 | FIPS 202 published example |
| KMAC256 | SP 800-185 official sample #4 |
| AES-256-GCM | GCM specification test case 14 |
| ML-KEM-768 key generation | NIST ACVP ML-KEM-keyGen-FIPS203 vector |
| ML-DSA-65 key generation | NIST ACVP ML-DSA-keyGen-FIPS204 vector |

Any failure places the module in an error state in which every cryptographic
service returns `PQC_ERR_SELF_TEST` and no output is produced. Recovery requires
a successful re-run. `tests/unit/test_selftest.c` verifies not only that the
tests pass but that the error state genuinely blocks every entry point,
including when the arguments are themselves invalid.

Conditional self-tests required by FIPS 140-3 that are **not** implemented:
pairwise consistency on freshly generated key pairs, and a continuous random
number generator test. Both are listed in section 13.

## 11. Life-cycle assurance

Development is version-controlled; every change carries its tests. The test
suite runs under `ctest` on macOS/arm64 and Debian/aarch64, under
AddressSanitizer and UndefinedBehaviorSanitizer, and under ThreadSanitizer for
the concurrent paths. Correctness of the algorithm path is pinned to NIST ACVP
vectors; `tools/kat_evidence.sh` regenerates the evidence table by running them.

The RTL is checked by `tools/rtl_lint.sh` (Verilator `-Wall`, every module as its
own top level, plus an Icarus Verilog pass) and by a cocotb regression against
independent oracles. Waivers are listed with reasons in
`hardware/rtl/lint_waivers.vlt` and matched per file and signal, so a new
occurrence of the same class is not silently covered.

Delivery is source-only. There is no signed release artefact, no formal
configuration-management record, and no operator guidance document beyond the
repository documentation.

## 12. Mitigation of other attacks

None claimed beyond the constant-time properties described in section 8 and in
[constant-time.md](constant-time.md).

## 13. Gaps relative to a validation submission

Each item below would independently block a submission. They are listed so the
document is not read as a statement of readiness.

1. **No device binding.** The key derivation root is a compiled-in constant
   whose literal text says so. A real device sources it from eFUSE, BBRAM, or a
   PUF.
2. **~~No entropy source inside the boundary.~~ Closed.** A ring-oscillator noise
   source with SP 800-90B continuous health tests runs inside the boundary; measured
   min-entropy is 0.871234 bits/sample. The **host** software still seeds from
   OpenSSL — Random bits come from the host
   operating system through OpenSSL.
3. **No module integrity check.** Nothing verifies the module image before use.
4. **Conditional self-tests missing.** No pairwise consistency test on generated
   key pairs, no continuous RNG test.
5. **No physical security.** The boundary is enforced logically (the AxPROT gate),
   not physically. There is no tamper-evident enclosure, no tamper response, no
   environmental failure protection. A `tamper` input exists in the RTL and zeroizes
   the key vault when asserted, but nothing is wired to it.
6. **No algorithm certificates.** ACVP vectors are run locally; that is evidence
   of correctness, not a certificate.
7. **Single-writer audit log.** The audit module does not lock the file.
8. **Unkeyed Shamir share checksums.** They detect corruption, not tampering.
9. **SO PIN lockout not enforced.** Failures are counted but the slot is not
   locked, because locking it would brick the device; recovery is by M-of-N.
10. ~~**No whole-core hardware implementation of ML-DSA.**~~ **Closed — this
    entry was wrong** (registry DOC-1). ML-DSA 44/65/87 KeyGen/Sign/Verify have
    whole cores behind `mldsa_axi` (slot 6) and match the NIST ACVP vectors
    byte-for-byte **on silicon** (32/32 on the board; see `board/logs/` and
    `docs/TESTING.md`). ML-KEM 512/768/1024 and the symmetric cores
    (AES-128/256, SM4, SM3) likewise match their standard vectors on silicon.

    > The stale wording said "operators only, not chained into whole
    > operations", which was true of an earlier stage and contradicted the
    > README, `TESTING.md` and the board logs. Three further copies of it are
    > fixed alongside (`docs/ARCHITECTURE.md`, the Chinese security policy, and
    > `zynq-port.zh-CN.md` §B.5). Understating what has been demonstrated is a
    > smaller sin than overstating it, but in a submission-shaped document it is
    > still a false statement — and this one would have invited the reviewer to
    > ask for evidence that already exists.

## Mapping to GM/T 0028-2014

GM/T 0028-2014 is aligned with ISO/IEC 19790:2012, so the section structure
corresponds closely. The table records where each requirement is addressed in
this document.

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

Two differences matter for a GM/T 0028 submission specifically and are not
addressed by this module: the standard's approved algorithm list is the Chinese
commercial cryptography suite (SM2, SM3, SM4, ZUC). **This sentence used to say
the module implements none of them, which was wrong**: SM4 and SM3 are both in the
PL and have been checked on the board against GB/T 32907 A.1 and GB/T 32905 A.1.
What is missing is **SM2 and ZUC**, neither of which has a core or a software
implementation; SM2 is the largest gap for GM/T certification. The gap list lives
in [API.md](../API.md). The module
implements; and the randomness requirements reference GM/T 0005 rather than
SP 800-90B, so the noise source health tests in `hardware/rtl/trng/` would need
to be re-derived against that document's thresholds.
