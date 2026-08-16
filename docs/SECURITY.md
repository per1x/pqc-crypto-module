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
   `SECURE_ONLY=1` accepts only secure transactions; everything else reads back
   zero and has its writes discarded, with no bus error raised (RAZ/WI).
2. **`axi4lite_xbar` decodes fully.** Aperture high bits, slot number, in-slot
   offset high bits and 32-bit alignment must all hold, or the transaction is
   refused in place — same RAZ/WI response. There are no mirror addresses.
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

### Root can still drive the hardware — a residual capability we knowingly keep

The EL3 SiP exposes whitelisted MMIO reads and writes, with the operation
sequence assembled by a **normal-world** daemon. The normal world cannot *read*
key material (the vault has no read path in RTL), but root can still load, erase
and use key-vault slots — it can **use** the keys, it just cannot **see** them.

⚠️ **Narrowing the whitelist does not close this.** The root cause is not that
the whitelist is too wide but that **the SiP does not authenticate its caller**
(independent review, M4): EL3 cannot tell "the daemon is calling" from "another
root process is calling". As long as root can issue an SMC, root can do anything
the daemon can; narrowing the whitelist only locks the daemon out as well.

| Approach | What it buys | Why not in this version |
|---|---|---|
| Operation-granularity SiP ("decapsulate this ciphertext with handle H" rather than "write this word to this address") | root can no longer assemble arbitrary register sequences | It moves the whole hardware sequence into EL3, **where there is no exception handling** — one refused bus access or one poll that never completes wedges a core, and the board can only be power-cycled. We measured exactly that with a single write on 2026-08-17. And it still does not authenticate the caller: root can invoke those operations too |
| Move the driver into a secure-world TA (`tee/ta/` already has the skeleton), leaving the normal world to submit operation requests | This is the shape that actually keeps root out | An architectural change, out of scope for this round |

**So the honest statement for this version is**: the cryptographic boundary stops
you from **reading** private keys, not from **using** them. An attacker with root
in the normal-world kernel can still command the hardware to compute. Closing
that needs a secure-world TA, not a narrower whitelist.

## What has been demonstrated

All on the real device.

**The gate works, in both directions.** The secure world (EL3, `AxPROT[1] = 0`)
reads a `SECURE_ONLY=1` core and gets `VERSION = 0x00010000`. The normal world
(EL1-NS, `AxPROT[1] = 1`) is refused at the same address — while that *same*
normal world reads a `SECURE_ONLY=0` core successfully. Both halves are needed:
the second is what rules out "the address was simply unreachable".

> **How refusal is observed changed after this measurement was taken.** The run
> above predates the RAZ/WI change and recorded DECERR/SIGBUS. The firewall now
> answers a refusal with data `0` and no bus error, so on the current bitstream
> the same experiment reads **0** where `0x00010000` lives.
>
> The new criterion is the stronger of the two, which is why it was adopted
> rather than merely tolerated: DECERR cannot distinguish "the gate is closed"
> from "there is nothing at this address at all", since an empty address also
> answers DECERR. Reading `0` where a nonzero constant lives proves **both**
> halves at once — the transaction reached that slave, and the value was
> withheld.
>
> **Re-running the two-directional measurement on the RAZ/WI bitstream is
> pending.** Simulation covers the new observable.

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
| ~~**ML-KEM private keys leave the hardware**~~ **fixed** | `mlkem_axi` now has an on-chip private-key vault: `MODE.DK_TO_SLOT` sends `dk` straight into it, `OUT_LEN` stops at the length of `ek`, and `DK_FROM_SLOT` decapsulates by slot. ML-DSA's `sk` works the same way. **Verified on the board 2026-08-17**: seeking the read cursor into the sk region returns all zeros, while the signature produced from the slot is byte-identical to the one produced when software supplies sk. The non-vault path remains for factory verification (ACVP checks the private key) and is closed in the delivery posture by a one-way latch |
| ~~**No device binding**~~ **built, off by default** | The derivation root can be bound to this chip's Device DNA (`src/crypto/kdr_dna.c`). DNA is exposed through a **read-only** window in the BL31 whitelist (`0xFFCA0050-5C`) — reading it directly from EL1 is a bus error. Both retrieval paths run on real hardware: `/dev/secmmio` when the library runs on the board (`board/src/dna_probe`), and `OP_DEVICE_DNA` over the wire when it runs on a remote host (`tools/dna-bind-check`, exercised from macOS against the board). ⚠️ **DNA is not a secret** — anyone with JTAG can read it — so the provider sets `device_bound=1` and `hardware_backed=0`: this buys anti-cloning, not confidentiality. The stub remains the default; `PQCHSM_KDR=device-dna` switches, because changing the root makes an existing keystore unopenable with symptoms identical to tampering. See `board/logs/RESULT_dna_bind.txt` |
| **No secure boot** | **The BBRAM key is now burned (2026-08-17, hardware CRC verified), but a BOOT.BIN encrypted with it does not boot on this board — see the section below.** The PL configuration is still loaded at runtime, not from a signed golden image |

> ### ⚠️ BBRAM is **not** a trust root — say this before building it
>
> On ZynqMP these are three separate things, and conflating them produces an
> overstated claim:
>
> * **BBRAM** holds an **AES-256 encryption key**: battery-backed, rewritable,
>   JTAG-programmable. It buys **confidentiality and integrity** (AES-GCM) for the
>   boot image.
> * **The authentication root (RSA) lives in eFUSE**: the PPK hash plus `RSA_EN`.
>   **Without burning eFUSE the BootROM does not enforce authentication** — an
>   attacker swaps in an image declared as neither encrypted nor authenticated and
>   the BootROM boots it happily.
> * `ENC_ONLY` is the same story: without eFUSE the BootROM does not enforce
>   encrypted boot either.
>
> **So BBRAM alone gives confidentiality and integrity, not resistance to
> replacement.** Real replacement resistance requires burning eFUSE, and eFUSE is
> irreversible with exactly one board available — **permanently excluded here**.
>
> After BBRAM the accurate statement is: "any root user swapping a bitstream
> replaces the whole cryptographic boundary" has been downgraded to "this now
> requires physical access and the ability to rebuild the entire image." It is
> **not** "a hardware trust root has been established" — that would be a fresh
> overstatement of the same kind the independent review flagged as H5 and H6.
>
> Two deployment costs come with it: the BBRAM key becomes a new key-management
> problem of its own, and **BBRAM is battery-backed — a dead battery means the
> board will not boot.**

> ### What actually got done, 2026-08-17: burned, but it will not boot
>
> Working:
>
> * `boot/persist/build_bbram_boot.sh` produces an encrypted BOOT.BIN with two
>   self-checks: no plaintext strings survive in the leading region, and the boot
>   header's key-source word is `0x3A5C3C5A`. (**These two constants are very easy
>   to swap**: `0x3A5C3C5A` is BBRAM, `0xA5C3C5A3` is eFUSE. The basis is not
>   memory but a controlled build — same bif, only `[keysrc_encryption]` changed.)
> * The key was burned over JTAG with XilSKey's `xilskey_bbramps_zynqmp_example`;
>   it returned `Status = 00000000`, meaning the **hardware CRC check passed**.
> * `bootgen -read ... bh` decodes a structurally correct boot header.
>
> **Not working: with the encrypted image in a non-golden slot and multiboot set,
> the serial console emits zero bytes — not even the FSBL banner. The failure is
> in the BootROM, decrypting the FSBL.**
>
> The attribution is measured, not assumed:
>
> | Experiment | Slot | Reboot path | Result |
> |---|---|---|---|
> | Plaintext twin (**byte-identical to golden**) | 6 | sysrq | **boots** |
> | Encrypted, key in natural order | 6 | sysrq | dead |
> | Encrypted, eight 32-bit words reversed | 6 | sysrq | dead |
> | Encrypted, all 32 bytes reversed | 6 | sysrq | dead |
> | Encrypted, bytes swapped within each word | 6 | sysrq | dead |
>
> Ruled out by those runs:
>
> * **Not the known flakiness of warm-rebooting into a non-zero slot** — the same
>   slot and path boot a plaintext image, and in the encrypted runs the board died
>   *at* slot 6 rather than falling back to golden, so slot 6 was genuinely read.
> * **Not key retention** — one run burned BBRAM and booted the encrypted image in
>   the same JTAG session, one system reset apart. Same failure.
> * **Not key byte order** — `ConvertStringToHexLE` does reverse the whole 32-byte
>   buffer, but **all four possible orderings were tried and all four failed**;
>   that search space is exhausted.
>
> What remains suspect is a bootgen 2020.1 encrypted-bif detail or this silicon's
> BootROM behaviour. **This project stops here.** Each attempt needs JTAG and takes
> the board down, and **encrypted boot is required by neither the demo nor the
> certification posture** — the delivery boundary rests on the one-way latches and
> the SiP whitelist, not on boot encryption.
>
> Board state for whoever picks this up:
>
> * BBRAM **holds a key** (`/home/build/bbram_ws/bbram_aes.key` on the build
>   machine, chmod 600). It currently **does nothing**: golden's boot header is
>   plaintext (`enc=0`) and `ENC_ONLY` is unblown, so the BootROM never consults
>   BBRAM. Harmless to leave, but it hardens nothing.
> * `BOOT0006.BIN` has been **deleted** from the SD card. Never leave an image that
>   cannot boot sitting in a slot — anyone who points multiboot at it needs JTAG or
>   a power cycle to get back.
> * The golden slot was never touched and **the board was never power-cycled**:
>   every recovery was a JTAG write of `CSU_MULTI_BOOT=0` plus a system reset,
>   five for five.
| **No module integrity check** | Nothing verifies the module image before use |
| **OP-TEE's secure memory is open to the normal world** | **Measured 2026-08-17**: `devmem 0x60000000` from Linux userspace returns `0xAA0003F3` — an AArch64 instruction, i.e. OP-TEE's secure-world code. The boot chain on this board configures **no XMPU_DDR regions at all** (all six instances read CTRL=0xb, default-allow, zero enabled regions) and XPPU is off (CTRL=0). "Keys held by OP-TEE are protected" is therefore **false on this board**. Two attempts to fix it are recorded in `board/logs/RESULT_protunit.txt`; neither took effect, and the work is stopped |
| **No physical security** | Logical enforcement only. The `tamper` input exists in RTL and zeroizes the vault; nothing is wired to it |
| **Conditional self-tests missing** | No pairwise consistency test on generated key pairs, no continuous RNG test |
| **No SM2** | SM4 and SM3 are implemented; the SM2 asymmetric algorithm is not |
| ~~**ML-DSA is operators only**~~ **fixed** | Chained into a full core and built into the bitstream (slot 6). All three parameter sets' KeyGen/Sign/Verify **matched ACVP byte for byte on the board on 2026-08-17**, in both bitstream forms; see `board/logs/` |
| **No algorithm certificates** | ACVP vectors are run locally. That is evidence of correctness, not a certificate |
| **Single-writer audit log** | The audit module does not lock the file |
| **Unkeyed Shamir share checksums** | They detect corruption, not tampering |
| **Keystore rollback protection only stops swapping one file** | The header carries an `epoch` incremented on every write (covered by the MAC), the highest epoch seen is kept in a sibling `<keystore>.epoch` file, and loading an older one is **refused**. That stops "take the SD card and copy an old keystore back" — the old snapshot's own MAC is valid, so the MAC cannot catch it. **But an attacker who rolls both files back consistently still gets through**: real anti-rollback needs monotonic storage the attacker cannot write (an eFUSE counter, RPMB, TPM NV), and this board has none |
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
