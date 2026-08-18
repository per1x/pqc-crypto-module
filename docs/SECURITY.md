**English** · [中文](SECURITY.zh-CN.md)

# Security model

> **For the delivered conclusions see [FINAL-REPORT-2026-08-17.zh-CN.md](FINAL-REPORT-2026-08-17.zh-CN.md).**
> Where this document conflicts with it, the final report wins — especially the
> encrypted-boot and XMPU sections.

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

> ### BBRAM encrypted boot, 2026-08-17: **blocked by board hardware (no `VCC_BATT` cell)**
>
> **Up front: the image is correct, the key is correct, the bif and the whitelist
> are correct. The one thing not done is the POR (physical power cycle) that must
> follow BBRAM programming — and house rules say I do not cut power to this board.**
>
> **What works**
>
> * `boot/persist/build_bbram_boot.sh` produces an encrypted BOOT.BIN with two
>   self-checks: no plaintext strings in the leading region, and a boot-header
>   key-source word of `0x3A5C3C5A`. (**These two constants are easy to swap**:
>   `0x3A5C3C5A` is BBRAM, `0xA5C3C5A3` is eFUSE. The basis is a controlled build —
>   same bif, only `[keysrc_encryption]` changed — not memory.)
> * The key was burned over JTAG with XilSKey's `xilskey_bbramps_zynqmp_example`:
>   `Status = 00000000`, i.e. the **hardware CRC check passed**.
> * The bif matches the official example from `bootgen -bif_help keysrc_encryption`
>   word for word.
> * The `.nky` is **the one bootgen generated itself** (`Device xczu3eg`, `Key 0/IV 0`
>   plus `Key 1/IV 1`), removing every hand-written-format ambiguity.
>
> **The cryptography is right — proved offline, not argued**
>
> Pulling the image off the board and decrypting it locally: with nky `Key 0` in
> natural order and the boot-header IV, at `fsbl_sourceoffset = 0x2800`, a
> **48-byte secure header plus 16-byte GCM tag verifies**. The recovered plaintext
> is exactly a well-formed ZynqMP secure header:
>
> ```
> 00000000…00000000            32 zero bytes = no key rolling for the next partition
> 7c301006c241950d34f84d95     12 bytes = IV 1 from the nky
> b87e0000                     4 bytes LE = 0x7eb8 words = 129760 bytes = pmufw length
> ```
>
> ⚠️ Several rounds were wasted before this because the secure header was assumed
> to be 64 bytes. **XilSecure's own header says `XSECURE_SECURE_HDR_SIZE = 48`.**
> Switching to 48+16 verified on the first try. **Do not infer the format — read
> the Xilinx source that does the decryption** (`xilsecure_v4_0/src/zynqmp/xsecure_aes.h`).
>
> **Where it is stuck: `BBRAM_STS.PGM_MODE` stays 1 after programming**
>
> Bit 0 of `0xFFCD0000` (`PGM_MODE`) latches at 1 once the key is programmed.
> None of these clear it:
>
> | Tried | Result |
> |---|---|
> | XilSKey's own `PrgrmDisable()` (writes 0 to `0xFFCD0008`) | ran; `PGM_MODE` still 1 |
> | JTAG write of 0 to `0xFFCD0008` | still 1 |
> | JTAG `rst -system` | still 1 |
> | Linux reset through the PMU **system** scope (`shutdown_scope=system`) | still 1 |
>
> While that latch is set the CSU will not hand the BBRAM key to the BootROM, so
> the encrypted FSBL cannot be decrypted — which is exactly what is observed: zero
> serial output, not even the FSBL banner, and the APU never released from reset.
>
> **The one untried step is a POR (physical power cycle)** — which is also Xilinx's
> documented flow: program BBRAM, power-cycle, then boot the encrypted image. XSDB's
> `rst -type ps-por` is Versal-only; ZynqMP has no software POR path, and the PMU's
> system-scope reset was tried and is not enough.
>
> **For whoever picks this up: cut power once, then**
>
> ```
> # after the board comes back (golden, plaintext, always boots):
> ssh root@<board> "cp /media/sd-mmcblk1p2/hsm/BOOT_ENC2.BIN /media/sd-mmcblk1p1/BOOT0006.BIN; sync"
> # set multiboot=6 over JTAG, then reboot from Linux
> ```
>
> Both outcomes are informative and must be recorded honestly:
>
> * **It boots** → POR cleared `PGM_MODE` and BBRAM is battery-backed → encrypted
>   boot works here.
> * **It does not boot and BBRAM reads zeroized** → this AXU3EG has no cell on
>   `VCC_BATT`, so **the key must be re-burned after every power cycle** and
>   encrypted boot has no practical value on this board. (Whether `VCC_BATT` is
>   populated has **never been verified** in this project; this step answers it.)
>
> **Board state right now (safe)**
>
> * Golden `BOOT.BIN` is **plaintext** and `ENC_ONLY` is unblown, so the BootROM
>   never consults BBRAM — **the board can be power-cycled at any time and will come
>   up**. `CSU_MULTI_BOOT = 0`.
> * The encrypted image sits at `/media/sd-mmcblk1p2/hsm/BOOT_ENC2.BIN`, **not in any
>   boot slot**. Never leave a non-booting image in a slot — anyone who points
>   multiboot at it then needs JTAG or a power cycle.
> * The key now in BBRAM (bootgen's `Key 0`) currently **does nothing**: harmless to
>   leave, but it hardens nothing.
> * The golden slot was never touched and **the board was never power-cycled**:
>   every recovery was a JTAG write of `CSU_MULTI_BOOT = 0` plus a system reset.

> ### 2026-08-17 (second session), PUF black-key boot: **root cause narrowed to one word; corrected image built, board test pending a power cycle**
>
> First, a correction to this document: it previously said PUF black keys "depend on"
> eFUSE burning and were permanently excluded. **That was wrong.**
> `bootgen -bif_help fsbl_config` defines `bh_auth_enable` as "RSA authentication of
> the bootimage will be done **excluding the verification of PPK hash and SPK ID**",
> and AMD's Embedded Design Tutorial states that PUF boot-header mode
> "**does not require programming of eFUSEs**". This path is viable under our rules.
>
> **Three defects explain the earlier BootROM rejection; each has an authoritative source:**
>
> | # | Defect | Authority | Measured in the old image |
> |---|---|---|---|
> | ① | bif missing `puf4kmode` — bootgen **defaults to 12K**, registration was 4K | `bootgen -bif_help fsbl_config`: "(Default is 12k bit)"; `xilskey_puf_registration.h:148` = `XSK_PUF_MODE4K` | boot header `0x44` bits `[17:16]` = **0** (should be 3) |
> | ② | bif missing `shutter=0x0100005E` | `xilskey_eps_zynqmp_puf.h:72` `XSK_ZYNQMP_PUF_SHUTTER_VALUE = 0x0100005e` | boot header `0x6c` = **`0x01000020`** (bootgen default) |
> | ③ | **The last helper-data word was corrupted by the previous round's "fix"** | `xilskey_eps_zynqmp_puf.c`: `Aux = (PufStatus & AUX_MASK) >> 4` but `SyndromeData[385] = (PufStatus & AUX_MASK) << 4` — **eight bits apart** | wrote `0x00864FE2`; correct value is **`0x864FE200`** |
>
> ③ is a regression caused by a "fix", and is worth recording in full. The first
> round wrote the 386 words of `PufInstance.SyndromeData[]` **verbatim** — which was
> correct; the library itself places CHASH at `[384]` and `AUX<<4` at `[385]`. The
> author then searched the image for `0x00864FE2`, did not find it, concluded "AUX is
> missing", and rewrote the file as "384 words + CHASH + AUX". **The search was bound
> to fail** — the value that belongs there is `0x864FE200`. A false negative damaged
> correct data. Corroboration: `word[383]` and `word[384]` are *both* CHASH, which is
> exactly what the library does (the 4K read loop stops at `Index=383`, so the last
> word read *is* CHASH, then it is copied to `[384]`) — proving the file is not
> misaligned and only the final word was wrong.
>
> **Two board runs this session (both in non-golden slots; golden untouched throughout):**
>
> | Variant | Config | BootROM behaviour | Cost |
> |---|---|---|---|
> | A | `pufhd_bh, puf4kmode, shutter=…`, helper data still bad | `MULTI_BOOT` 6 → **7**, kept searching | None — slot 7 was pre-loaded with a copy of `BOOT.BIN` as a safety net |
> | B | A + `bh_auth_enable` + RSA-4096 on all partitions | **Halted**, `MULTI_BOOT` stuck at 6, APU held in reset | **Board needs a POR** |
> | C | A + **corrected helper data** | **Not yet tested** | — |
>
> Variant C is built and passes every offline self-check
> (`boot/persist/build_puf_boot.sh`, md5 `7142f40e26d4b0088374783b2c7e12d9`),
> including a word-by-word comparison of all 386 helper-data words in the image.
>
> ⚠️ **Wording**: this is "root cause supported by authoritative sources, corrected
> image built and self-checked" — **not** "secure boot works". C has never booted.
>
> ⚠️ Even if C succeeds the ceiling is unchanged: without eFUSE there is no
> `ENC_ONLY` / `RSA_EN`, so the BootROM enforces neither encryption nor
> authentication. PUF buys **confidentiality plus device binding**, not a
> replacement-resistant trust root.

> ### 2026-08-17 (second session), why the XMPU does not stop the APU: **gating is not its job**
>
> The previous round recorded this as "needs UG1085's DDR-path architecture".
> It is settled, and the answer has nothing to do with the DDR path.
>
> **Authority**: XAPP1320, *Isolation Methods in Zynq UltraScale+ MPSoCs*, v3.0
> (2020-04-30), page 10:
>
> > "If an illegal transaction is attempted, the XMPU asserts AxUser[10] but the
> > transaction **is passed to the memory controller**. This mechanism is referred to
> > as poison by attribute. **The transaction is gated by the end point, not the XMPU
> > itself.**"
>
> The XMPU only *detects and marks*; the endpoint (the DDR controller) is what drops
> the transaction. In our configuration poisoning was never enabled:
>
> ```
> DDR0..5  CTRL   = 0x0000000b   → bit2 POISONCFG = 0
> DDR0..5  POISON = 0x00000000   → ATTRIB(bit20) = 0, BASE = 0
> ```
>
> That makes the observations self-consistent: the regions are right, detection is
> live (DMA-master violations latched), but nothing gates, so data still returns.
>
> **Three hypotheses ruled out against Xilinx's own definitions:**
>
> * **Bit layout is correct.** `xddr_xmpu0_cfg.h`: `[0]EN [1]RDALWD [2]WRALWD
>   [3]REGNNS [4]NSCHKTYPE`. Xilinx's QEMU model `hw/misc/xlnx-xmpu.c:205-228` gives
>   the strict-mode test `sec_access_check = (sec != regionns)` — a non-secure access
>   to a `REGNNS=0` region is a security violation, exactly what we want.
> * **Address granularity is correct.** `ALIGNCFG=1` does not change the field scale:
>   `hw/misc/xlnx-zynqmp-xmpu.c:283-292` always does `xr->start <<= 12`; lines 327-328
>   merely *align* the region to 1 MB. Confirmed on hardware: `ERR_ADDR = 0x0006002a`
>   → `0x6002a000`, inside the region.
> * **`MASTER = 0` is correct.** Same file, line 330:
>   `(mask & id) == (mask & master_id)` — a zero mask matches every master.
>
> ### ✅ 2026-08-17 (wrap-up session) — **done: poison-by-attribute, applied at boot**
>
> The "next step (not done)" above has been done, and it is now part of the default form.
>
> BL31 writes `POISON.ATTRIB` (bit 20) on all six XMPU_DDR instances from EL3.
> **Only `POISON` is touched, never `CTRL`** — `CTRL` stays `0x0b`, for two reasons that
> both matter: (1) `DEFRDALWD`/`DEFWRALWD` govern accesses matching *no* region, so
> clearing them denies all of DDR; (2) `CTRL.POISONCFG` selects poison-*by-address*
> redirection to `POISON.BASE`, and `BASE = 0` — one illegal **write** would land at
> physical address 0. So this uses the mode XAPP1320 explicitly recommends: by attribute.
>
> **Measured on the board (default image, active at power-on, no manual step):**
> ```
> devmem 0x60000000 → Bus error      OP-TEE core carve-out, fenced
> devmem 0x10000000 → 0xEDFE0DD0     outside the region, unaffected
> devmem 0x70000000 → unaffected     OP-TEE shared memory, deliberately not fenced
> ```
> Clearing poison and re-reading the same address returns `0xAA0003F3` — an OP-TEE
> AArch64 instruction. That same-address comparison is the disproof showing root
> really could read secure memory before. `ERR_MASTER = 0xa0` (APU) and
> `ISR = 0x8[SECURTYVIO]`: detection *and* gating now both hold.
>
> **The read-only experiment above is no longer needed** — poisoning itself answered it:
> the access does reach XMPU_DDR and is detected; only gating was missing.
> `DDR0/3/4/5` error registers stay zero simply because those four ports (LPD, HP0-3,
> FPD_DMA) carry no traffic to this address range — not because nothing arrives.
>
> Evidence: `board/logs/RESULT_xmpu_persist.txt`.
> ⚠️ What this blocks is **only "normal world reads OP-TEE's core segment"**. The
> residual-capability entry "root can still drive the hardware" below is **unchanged**.

| **No module integrity check** | Nothing verifies the module image before use |
| ~~**root can still read OP-TEE's secure memory**~~ — **closed 2026-08-17 (wrap-up session)** by poison-by-attribute applied at boot; see the ✅ section below. The original record is kept here to show how large the hole was | **Measured 2026-08-17**: `devmem 0x60000000` from Linux userspace returns `0xAA0003F3` — an AArch64 instruction, OP-TEE's secure-world code. The vendor boot chain configures **no XMPU_DDR regions at all**. BL31 now programs one secure region per XMPU_DDR instance (all six) covering OP-TEE's core carve-out `0x6000_0000+0x1000_0000`, `cfg=0x17`, with the bit layout taken from Xilinx's own `xddr_xmpu0_cfg.h`. **It does detect**: `ERR_MASTER` latched `0xa0` and `0xac` — two non-secure DMA masters, with ISR set. **It does not stop the APU's own non-secure reads**: comparing the same addresses before and after, `devmem` returns byte-identical values. So "keys held by OP-TEE are protected" is **still false here**; what closed is *detection* on the rogue-peripheral-DMA path. **Root cause established 2026-08-17 (second session) — see the section below: the XMPU does not gate transactions at all.** Evidence in `board/logs/RESULT_protunit.txt` |
| **No physical security** | Logical enforcement only. The `tamper` input exists in RTL and zeroizes the vault; nothing is wired to it |
| **Conditional self-tests missing** | No pairwise consistency test on generated key pairs, no continuous RNG test |
| **No SM2** | SM4 and SM3 are implemented; the SM2 asymmetric algorithm is not |
| ~~**ML-DSA is operators only**~~ **fixed** | Chained into a full core and built into the bitstream (slot 6). All three parameter sets' KeyGen/Sign/Verify **matched ACVP byte for byte on the board on 2026-08-17**, in both bitstream forms; see `board/logs/` |
| **No algorithm certificates** | ACVP vectors are run locally. That is evidence of correctness, not a certificate |
| **Single-writer audit log** | The audit module does not lock the file |
| **Unkeyed Shamir share checksums** | They detect corruption, not tampering |
| **Keystore rollback protection only stops swapping one file** | The header carries an `epoch` incremented on every write (covered by the MAC); where the anchor lives is chosen by a provider (`pqchsm/rbanchor.h`), and loading a file older than the anchor is **refused**. The default provider keeps the anchor in a sibling `<keystore>.epoch` file, which **is not real anti-rollback**: an attacker who rolls both files back consistently still gets through (`test_keystore` pins this weakness with a dedicated case). Real anti-rollback needs a monotonic value the attacker cannot rewind — the RPMB implementation exists in the tree and this board's RPMB **is usable** (the key was deleted by mistake, then recovered and confirmed); see the 2026-08-18 section below |
| **SO PIN lockout not enforced** | Failures are counted but the slot is not locked, because locking it would brick the device; recovery is by M-of-N |
| **`CKM_HASH_ML_DSA_*` not implemented** | PKCS#11 multi-part signing buffers the whole message instead of streaming a digest |
| **Host RNG is OpenSSL** | The PL entropy source is inside the boundary, but the host software still seeds from the operating system through OpenSSL |
| **Hold-timing margin is thin** | Effective hold margin is 0.110 ns after explicit management. A hold violation cannot be fixed by slowing the clock, so this number must be re-checked whenever the RTL changes; the implementation flow aborts below a 0.050 ns floor |

Two items are blocked on JTAG hardware rather than on design work: persisting
the PL configuration into the golden `BOOT.BIN`. BBRAM-backed secure boot is blocked
by board hardware (no `VCC_BATT` cell). **Only one item is permanently excluded on
this board: eFUSE burning** (irreversible, single board) — which is why a
replacement-resistant trust root (`ENC_ONLY` / `RSA_EN`) is out of reach for good.
**PUF black keys do *not* depend on eFUSE** (this document previously said they did;
corrected above), and their root cause is now identified.

A FIPS 140-3 / GM/T 0028 style security policy draft, with its own gap list, is
in [reference/security-policy.md](reference/security-policy.md).

---

## 2026-08-18: remote channel, rollback anchor, build profile

This section states the **current** position on three things. Where it conflicts with
anything earlier in this document, this section wins.

### 1. The remote port moved from "plaintext TCP + shared token" to **mTLS**

**What it used to be** (the TCP front end of `service/pqchsm_fpgad`): the first frame
had to be `OP_AUTH` carrying a static pre-shared token, compared in constant time.
Three problems:

1. **The token was on the wire in the clear.** One packet capture on the same segment
   yielded it, and it was static — capture once, own it forever. Constant-time
   comparison bought nothing here: the attacker never had to guess byte by byte.
2. **The whole session was plaintext and malleable.** Public keys, ciphertexts,
   signatures and plaintext data all travelled unprotected, with no integrity at all —
   a man in the middle could flip a verification result from "fail" to "pass".
3. **It was replayable.** The auth frame itself could be recorded and replayed; the
   server had no freshness check.

**Now**: the TCP connection runs inside **TLS 1.3** with **certificates on both sides**
(`SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT`), with the protocol version
pinned to 1.3 at both ends so there is nothing to downgrade to. Identity moves from
"knows a string" to "holds a private key".

**What this layer now stops**: any host on the segment, anyone who can capture,
anyone who can modify, anyone replaying a recording.

**What it still is not**: complete access control.
- No privilege separation and no per-operation authorisation — passing mTLS grants
  every operation.
- The ACL is CN-granular only (`pki/hsm_acl`, one per line; **absent file means any
  certificate signed by this CA is accepted**, a deliberate default explained in
  `service/pqcs_tls.h`).
- No revocation (no CRL/OCSP) — a leaked client certificate means re-issuing the CA.
- Rate limiting is a per-IP leaky bucket (8 failed handshakes / 60 s) that addresses
  availability (a single-threaded daemon being pinned by handshake spam), not
  confidentiality.

**The PKI is prototype grade**: the CA private key sits in a 0600 file on the
operator's machine, with no hardware protection, no intermediate CA, no revocation. It
is enough to replace "plaintext token" with "holds a private key"; it is **not** a PKI
in the institutional sense.

**The local UNIX socket is deliberately not wrapped in TLS**: it is 0600 and
host-local, so reaching it already means root; encrypting it stops nobody and only
adds a certificate for on-board troubleshooting tools to manage.

**Dependency cost**: both the daemon and the client link OpenSSL. The board build is
**not fully static** — linking this prebuilt OpenSSL 1.1.1d statically segfaults on
the target **before `main`** (a constructor of our own never gets to print; it happens
whenever `libssl.a` enters the link). The board build therefore links dynamically,
with the two `.so` files on the SD card in `hsm/lib/` and an `-rpath` pinning the
lookup path. The reasoning is in the header of `service/Makefile`.

**The clock is a real precondition** for this channel: certificates have validity
dates and this board's clock can fall behind `notBefore`. A TLS 1.3 rejection arrives
*after* the client considers the handshake done, so the symptom is "device info prints
garbage", not "authentication failed". Three mitigations: `demo_remote.sh --provision`
syncs the clock and writes the RTC; `hsm-boot.sh` refuses to leave the clock older
than the credentials on the SD card; the client performs one `OP_PING` round trip at
open to force such a rejection into the open.

Regression: `tools/tls_regress.sh` (ctest `tls_regress`) — one positive case plus
three negatives (certificate from a different CA / no client certificate at all / CN
not in the ACL). **The negatives are the point**; "it connects" only proves the
configuration is not broken.

### 2. Rollback anchor: abstracted, but **this board's RPMB is gone**

`include/pqchsm/rbanchor.h` turns "where the monotonic epoch lives" into a provider:

- `file` (default) — the sibling `<keystore>.epoch`. **Not real anti-rollback**, for
  the reason above.
- `rpmb` — the eMMC RPMB write counter: hardware-maintained, increment-only.

**Why the anchor is the write counter and not a value stored in RPMB**: the RPMB data
area can be rewritten at will by whoever holds the authentication key, including to a
smaller value. This board has no secret hardware root, so the RPMB key can only live
in a file — "the attacker has the key" is an assumption we must make. Anchoring on the
counter makes that assumption survivable: **they can push it forward, they cannot pull
it back**, so an old snapshot can never match again.

**Hardware finding (measured)**: this AXU3EGB has eMMC (`mmcblk0`, Q2J55L) with
`EXT_CSD[168] RPMB_SIZE_MULT = 32` → **4 MB RPMB**, `/dev/mmcblk0rpmb` present, and a
kernel that supports `MMC_IOC_MULTI_CMD`. **The hardware is sufficient.**

**There was an incident on this board and the key was nearly lost for good. The
account stays here:**

> While provisioning the RPMB authentication key on 2026-08-18, the `Program Key`
> request **actually succeeded**, but the tool read back a **stale response frame**
> (RPMB reads on this eMMC alternate between the correct response and the previous
> request's response) and reported "programming failed"; the key file, apparently
> unused, was then deleted. The conclusion drawn at the time was "RPMB permanently
> unusable".
>
> **That conclusion was wrong.** The key was later recovered from **freed blocks** on
> the SD card and confirmed cryptographically (`Read Write Counter` with HMAC and
> nonce-echo verification: the recovered key passed 3/3, a one-bit variant and fully
> random keys passed 0/8). RPMB is **usable**; the rollback anchor can land.
>
> The recovery method is worth recording. The key is `RAND_bytes(32)` and therefore
> **not recomputable** — only the bytes themselves could be found. But ext4 here has
> `inline_data` off and a 4096-byte block size, so those 65 bytes (64 hex characters
> plus a newline) **necessarily occupy a whole block, starting at its first byte**.
> Scanning the partition for "64 lowercase hex at a block boundary, immediately
> followed by a newline" left **exactly one** hit out of 1 824 256 blocks.
> ⚠️ A scan without the alignment criterion is useless: the partition holds 6882
> distinct 64-hex strings (KAT vectors and hashes in logs).

**The real lesson is not "be more careful next time"** — it is the rule below, which
now lives in the code.

### ⛔ Project rule: no more one-time / irreversible programming

**An irreversible action plus a state read that can lie is unacceptable.** RPMB reads
on this eMMC alternate with stale frames, which means "the device says it is not
programmed" **is not trustworthy**; on a device that cannot be read reliably, the risk
of an irreversible act is not a function of how careful the operator is. In the
incident above the tool, the criteria and the warnings had all been written — and it
happened anyway, because the failing step was outside what care can cover.

The rule is enforced **at compile time**, not with a longer runtime flag (a runtime
flag does not stop the "I know what I'm doing" moment):

- `rpmb_tool provision` is **not compiled in** unless `RPMB_ALLOW_PROVISION` is
  defined; running it only prints this rule and exits. Provisioning a **new** board
  requires editing and rebuilding (`-DRPMB_ALLOW_PROVISION=1`), making it a recorded
  decision rather than a command anyone can type.
- The same applies to **eFUSEs** (permanently excluded by this project), BBRAM latch
  bits, one-time latches that cannot be cleared, and any "burn it and it never comes
  back" bit.
- Correspondingly, **any irreversible decision taken on a device-state reading** must
  first establish that the reading is sound: RPMB responses are always validated on
  `req_resp` (read `0x0200` / write `0x0300` / program-key `0x0100`) and on the nonce
  echo, and **never on `result`** (precisely the field a stale frame corrupts); I/O
  failure and "wrong key" must return distinct codes — reporting the former as the
  latter is exactly the misjudgement behind this incident.

**Alternatives** (none of them landed on this board; recorded honestly):**Alternatives** (none of them landed on this board; recorded honestly):
- **TPM NV counter** — no TPM on this board and no usable SPI/I²C TPM header;
- **eFUSE counter** — burning eFUSEs is **permanently excluded** by this project
  (irreversible, single board);
- **A replacement eMMC or a second board of the same type** — the RPMB path works
  as-is, the code is already in the tree;
- **A remote anchor service** (fetch a monotonic ticket on every save) — trades the
  problem for "that server must always be reachable", which does not hold for an
  offline HSM but is viable for a networked deployment.

### 3. Build profile `PQC_PROFILE`: DEV and PRODUCTION

The old default was "fall back to the stub if no KDR provider is installed", and the
stub's root key is a **public constant compiled into the binary** (the literal itself
reads `NOT SECRET`). In other words, **by default the keystore's wrapping key was
public** and anyone with the SD card could decrypt the whole keystore offline — with
nothing anywhere in the code to say so.

There are now two explicit profiles (`cmake -DPQC_PROFILE=…`, default `DEV`):

| | DEV (default) | PRODUCTION |
|---|---|---|
| Stub root key | compiled in | **`#if`'d out entirely — not findable in the binary** |
| No provider installed | falls back to the stub | provider is NULL, every derivation fails |
| Startup gate | proceeds, with an explicit warning | requires `hardware_backed`, else **refuses to start** |

`tools/check_profile.sh` (ctest `profile_no_stub_kdr`) scans the PRODUCTION object file
for that literal **and carries a null control** (it must be findable in DEV) —
otherwise renaming the literal would make the check silently pass forever.

⚠️ **This board cannot reach PRODUCTION**: there is no secret hardware root (eFUSE
permanently excluded, BBRAM has no battery, PUF black key never worked), and the
device-dna provider is `device_bound=1` but `hardware_backed=0` (the DNA differs per
die so anti-cloning holds, but the DNA **is not a secret** — anyone with JTAG reads it
directly). A PRODUCTION build therefore refuses to start on this board, which is
**exactly what the switch is meant to express**, not a defect. Demos and regressions
run DEV.
