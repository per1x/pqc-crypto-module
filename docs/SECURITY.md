**English** · [中文](SECURITY.zh-CN.md)

# Security model

What the boundary is, what has been demonstrated on the device, and — at equal
length — what has not. To report a vulnerability, see [../SECURITY.md](../SECURITY.md).

This is a research prototype. Nothing here is certified, and it should not be
used to protect anything real.

- [The boundary](#the-boundary)
- [Threat model](#threat-model)
- [What has been demonstrated](#what-has-been-demonstrated)
- [What is not defended](#what-is-not-defended)
- [Limitations](#limitations)

## The boundary

**The cryptographic boundary is hardware.** The ML-KEM cores, the symmetric
cores (AES-128/256, SM4, SM3), the ring-oscillator entropy source, the key vault
and the access gate all live in FPGA fabric. The host software — keystore, slot
metadata, wrapping, PKCS#11 — runs *outside* it.

Three mechanisms make up the boundary:

1. **`axi4lite_firewall`** gates every slave on `AxPROT[1]`. A slave built with
   `SECURE_ONLY=1` accepts only secure transactions and answers everything else
   with DECERR.
2. **`axi4lite_xbar` decodes fully.** Aperture high bits, slot number, in-slot
   offset high bits and 32-bit alignment must all hold, or the transaction is
   refused in place. There are no mirror addresses.
3. **`key_vault` has no read path.** Keys go in over the bus and come out only
   on private `use_key` wires into the cipher cores. This is a structural
   property of the RTL, not a permission check.

The PL firewall is the **only** enforcement layer on this route. UG1085 v2.5
settles the question: `0x8000_0000` appears in no XPPU aperture (Table 16-10),
and FPD_XMPU is not on the `M_AXI_HPM0_LPD` path — the PL is reached "without
the FPD" (p. 1092). No PS-side protection unit covers the PL window at all.
That is not a gap in this design; it is the reason the decode has to be exact.

(Measured in passing: on this board the PS-side protection units are entirely
unconfigured — XPPU `CTRL = 0`, all 400 permission entries at reset defaults,
XMPU_OCM regions all disabled, and XPPU not locked. Irrelevant to the above:
even fully configured they would not reach the PL.)

## Threat model

**Defended against.** An attacker with normal-world (EL1-NS) code execution,
including root, who wants to *read* key material out of the module. They can
issue arbitrary AXI transactions through `/dev/mem` and can scan the whole
window; they cannot recover a symmetric key from the vault, and cannot read any
slave built with `SECURE_ONLY=1`.

**Not defended against.**

- An attacker with **secure-world (EL3) code execution**. EL3 is the trust root
  here; compromising BL31 compromises everything.
- An attacker who can **reconfigure the PL**. Loading a different bitstream
  replaces the boundary. Nothing on this board prevents it, because secure boot
  is not provisioned (see [Limitations](#limitations)).
- An attacker with **physical access**. There is no enclosure, no tamper
  response wired up, no environmental protection. The RTL has a `tamper` input
  that zeroizes the vault; nothing drives it.
- An attacker measuring **power or electromagnetic emissions**. Out of scope,
  deliberately — see below.
- An attacker who can **read host process memory**. The host stack is outside
  the boundary: plaintext key material exists in process memory there. Buffers
  are zeroed and `mlock`-ed where possible, and that is all.

**Root can still drive the hardware.** The EL3 SiP currently exposes whitelisted
MMIO reads and writes, with the operation sequence assembled by a normal-world
daemon. So the normal world cannot *read* key material, but root can still load,
erase and use key vault slots. Closing that requires an operation-granularity
SiP ("decapsulate this ciphertext with handle H", not "write this word to this
address"), which means a buffered state machine running in EL3 — where there is
no exception handling, so one bad fetch is a core wedged on the spot. It needs
its own verification approach and is not attempted here.

## What has been demonstrated

All on the real device.

**The gate works, in both directions.** The secure world (EL3, `AxPROT[1] = 0`)
reads a `SECURE_ONLY=1` core and gets `VERSION = 0x00010000`. The normal world
(EL1-NS, `AxPROT[1] = 1`) is refused at the bus with DECERR/SIGBUS — while that
*same* normal world reads a `SECURE_ONLY=0` core successfully. Both halves are
needed: the second is what rules out "the address was simply unreachable".

> **Which bitstream this evidence comes from, stated precisely.** The
> two-directional proof needs a `SECURE_ONLY=0` core to serve as the control, so
> it was taken on a build that has one. **The default (shipping) bitstream has
> no such core** — all four functional slaves are `SECURE_ONLY=1` and the normal
> world reaches none of them. On that build the measurement is one-directional
> by construction: 6 / 6 refused, with the secure world driving the full KAT
> suite successfully at the same time. Both results are real; neither is
> evidence for the other's configuration.

**Symmetric keys do not cross the boundary.** 256 bytes were scanned on each of
two slaves — 48 readable, 80 refused by the firewall. Not one of the key's four
words appeared anywhere, while the ciphertext produced with those keys was
byte-exact against GB/T 32907. Both halves are needed here too: neither "nothing
was found" nor "the cipher works" proves anything alone.

**Algorithm correctness is anchored to published vectors, not to this
project's own model.** ML-KEM 512/768/1024 KeyGen/Encaps/Decaps match NIST ACVP
byte-for-byte on silicon (20/20); AES against FIPS 197, SM4 against GB/T 32907,
SM3 against GB/T 32905.

**Entropy is measured, not assumed.** 1,048,576 **pre-conditioning** samples
were exported and assessed with SP 800-90B non-IID estimators: H = 0.871234
bit/sample. That measurement then set the health-test cutoffs. It had to:
the previous cutoffs, computed under an assumed H = 0.5, gave an APT trigger
probability of 5.5 × 10⁻⁵² — the test could never have fired, so every prior
"no APT alarm" record was worth nothing. See [TESTING.md](TESTING.md#entropy).

**Timing is flat on the implicit-reject path.** ML-KEM Decaps with a valid
ciphertext versus one with a single bit flipped, 200 runs each: median latencies
differ by 0.000 %. A real early exit would show an order-of-magnitude
difference, not a few percent.

Raw logs for all of the above are kept verbatim in [../board/logs/](../board/logs/).

## What is not defended

**Power and electromagnetic side channels are deliberately out of scope**, for
three reasons stated so this reads as a decision rather than an omission:

1. Validating a countermeasure needs a side-channel bench — oscilloscope, power
   and EM probes, TVLA methodology. Without one, implementing masking and being
   unable to demonstrate it works means treating "looks done" as "done", which
   is worse than not doing it.
2. First-order masking splits every intermediate into shares and needs
   randomness for each. On the ML-KEM datapath that would very likely exceed the
   ZU3EG's 71K LUT — 50.5 % is already used — and force a larger device.
3. Timing and power/EM are different attack surfaces. The constant-time work
   covers the first and cannot substitute for the second.

**Nothing is claimed about liboqs or OpenSSL.** Every ML-KEM and ML-DSA
operation on the host software path runs inside liboqs. A timing side channel
there is one here, and no check in this repository would find it.

**The constant-time audit is lexical, not compiler-level.** It cannot see a
branch the compiler introduces, a `cmov` it declines to emit, or a secret
spilled to a stack slot and reloaded. The empirical timing test covers the
comparison primitive; the rest of the source is covered by the scan only.

**Nothing is claimed about memory outside the process address space** — CPU
registers, swapped pages, or DRAM contents after power-off. `mlock` is
best-effort and unverified under memory pressure.

## Limitations

Each of these would independently block a validation submission. They are listed
so this document is not mistaken for a statement of readiness.

| | |
|---|---|
| **ML-KEM private keys leave the hardware** | `KeyGen` returns `ek ‖ dk` over AXI, because ACVP checking requires it. The daemon holds `dk` and gives applications only a handle, so it does not cross the *interface* — but "the private key never leaves the hardware" is false and is not claimed. Fixing it means adding private-key storage and handle-based use inside `mlkem_axi` |
| **No device binding** | The key derivation root (`src/crypto/kdr.c`) is a compiled-in constant whose literal text says so. A real device sources it from eFUSE, BBRAM or a PUF. eFUSE is irreversible and there is one board; BBRAM needs JTAG |
| **No secure boot** | BBRAM key provisioning needs JTAG. The PL configuration is loaded at runtime, not from a signed golden image |
| **No module integrity check** | Nothing verifies the module image before use |
| **No physical security** | Logical enforcement only. The `tamper` input exists in RTL and zeroizes the vault; nothing is wired to it |
| **Conditional self-tests missing** | No pairwise consistency test on generated key pairs, no continuous RNG test |
| **No SM2** | SM4 and SM3 are implemented; the SM2 asymmetric algorithm is not |
| **ML-DSA is operators only** | 13 modules verified against the reference model, not chained into KeyGen/Sign/Verify |
| **No algorithm certificates** | ACVP vectors are run locally. That is evidence of correctness, not a certificate |
| **Single-writer audit log** | The audit module does not lock the file |
| **Unkeyed Shamir share checksums** | They detect corruption, not tampering |
| **SO PIN lockout not enforced** | Failures are counted but the slot is not locked, because locking it would brick the device; recovery is by M-of-N |
| **`CKM_HASH_ML_DSA_*` not implemented** | PKCS#11 multi-part signing buffers the whole message instead of streaming a digest |
| **Host RNG is OpenSSL** | The PL entropy source is inside the boundary, but the host software still seeds from the operating system through OpenSSL |
| **Hold-timing margin is thin** | Effective hold margin is 0.110 ns after explicit management. A hold violation cannot be fixed by slowing the clock, so this number must be re-checked whenever the RTL changes; the implementation flow aborts below a 0.050 ns floor |

Two items are blocked on JTAG hardware rather than on design work: persisting
the PL configuration into the golden `BOOT.BIN`, and BBRAM-backed secure boot.
Two items are permanently excluded on this board: eFUSE burning (irreversible,
single board) and PUF black keys (which depend on it).

A FIPS 140-3 / GM/T 0028 style security policy draft, with its own gap list, is
in [reference/security-policy.md](reference/security-policy.md).
