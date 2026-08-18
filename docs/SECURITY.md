**English** · [中文](SECURITY.zh-CN.md)

# Security model

What the boundary is, what has been demonstrated on the device, and — at equal
length — what has not. To report a vulnerability, see [../SECURITY.md](../SECURITY.md).

This is a research prototype. Nothing here is certified, and it should not be
used to protect anything real.

- [⛔ Irreversible-operation red lines](#-irreversible-operation-red-lines)
- [The boundary](#the-boundary)
- [Threat model](#threat-model)
- [What has been demonstrated](#what-has-been-demonstrated)
- [What is not defended](#what-is-not-defended)
- [Limitations](#limitations)
- [Boot and the trust root](#boot-and-the-trust-root)
- [Memory isolation on DDR (XMPU)](#memory-isolation-on-ddr-xmpu)
- [The remote port](#the-remote-port)
- [Rollback anchor](#rollback-anchor)
- [Build profile: DEV and PRODUCTION](#build-profile-dev-and-production)

## ⛔ Irreversible-operation red lines

**This section takes precedence over everything else in this document.** Where
anything else conflicts with it, this section wins.

### The rule

On this AXU3EGB / ZU3EG board, **any one-time, irreversible programming is
forbidden**.

- It is **not only eFUSEs**. It is **every irreversible write** — fuses, OTP,
  one-time latch bits, permanent write-protect bits, on whatever device.
- Any irreversible action **requires the explicit consent of the board's owner
  (the user) first. The default answer is always "no".**
- **A session or contributor must not turn "evaluate" into "execute" on its own.**
  "Check whether this is possible" stops there; "then do it" is a separate
  decision that needs its own permission. Prior authorisation — including
  phrasings like "do it for real if you can" — **does not cover** this class of
  action.
- Unsure whether some bit belongs to this class? **Treat it as if it does, and
  ask.**

### Why the rule is closed by category, not by list

Because a list will always miss something, and the missing entry does not show
up as "the list was incomplete" — it shows up as a board that cannot be brought
back. So the criterion is the **category**: if a write cannot be undone, it is
inside the red line. The list below is **illustrative, not exhaustive**.

### The sharper point: an irreversible action plus a state read that can lie is unacceptable

There was an incident on this board on 2026-08-18 (see "Current state" below).
The tool, the criteria and the warnings had **all been written** — and it
happened anyway. The cause was not insufficient care: RPMB reads on this eMMC
**alternate with stale frames**, which means "the device says it is not
programmed" **is not a trustworthy statement**.

**On a device that cannot be read reliably, the risk of an irreversible act is
not a function of how careful the operator is.**

Two requirements follow:

- **Any irreversible decision resting on a device-state reading must first
  establish that the reading is sound.** RPMB responses are always validated on
  `req_resp` (read `0x0200` / write `0x0300` / program-key `0x0100`) and on the
  nonce echo, and **never on `result`** (precisely the field a stale frame
  corrupts); I/O failure and "wrong key" must return **distinct codes**.
- **Never delete the only copy of anything before the state is confirmed.** That
  step, not the programming itself, is what nearly made the incident permanent.

### One-time programming list (any hit is a red line; illustrative, not exhaustive)

**ZU3EG / ZynqMP eFUSEs** (authoritative enumeration: UG1085 ch. 12 and Xilinx
XilSKey):

| Class | Bits |
|---|---|
| Authentication root | `PPK0_HASH`, `PPK1_HASH`, `RSA_EN` |
| Encryption | AES device key, `ENC_ONLY`, `AES_RD_LOCK`, `AES_WR_LOCK` |
| Revocation | `SPK_ID` / SPK revocation bits, PPK invalidation (`PPK0_INVLD`, `PPK1_INVLD`) |
| Debug lockout | `JTAG_DISABLE`, `DFT_DISABLE` |
| Locking | `SEC_LOCK`, `PROG_GATE` |
| User area | `USER_0` … `USER_7` |
| PUF | syndrome (helper data), `CHASH`, `AUX`, `SYN_INVLD`, `SYN_WR_LOCK`, `REG_DIS` |

**eMMC**:

- the **RPMB authentication key** (`Program Key`) — one shot per device, and
  unreadable once programmed;
- the eMMC **permanent write-protect** bits.

**Catch-all**: any OTP, and any fuse whose name contains `*_LOCK`, `*_DISABLE`
or `*_EN` — treat as a red line on sight.

### Reversible alternatives (allowed; outside this section's scope)

| Mechanism | Why it is reversible |
|---|---|
| **BBRAM key** | Battery-backed, rewritable as often as you like |
| **QSPI reflash** | Ordinary flash, rewritable |
| **SD card** | Swap the file, or swap the card |
| **Bitstream reload** | `fpgautil` reconfigures the PL at any time |
| **Device DNA** | **Read-only** — physically cannot be written |

⚠️ One known trap: BBRAM is reversible but **this board's `VCC_BATT` has no
battery**, so its contents are lost on power-down. "Rewritable" holds;
"persistent" does not. See the BBRAM section of this document.

### Current state: this board's eMMC RPMB (2026-08-18 incident, **recovered**)

- **What happened**: `rpmb_tool provision`'s `Program Key` request **actually
  succeeded**, but the tool read back a **stale response frame**, reported
  "programming failed", and the key file — apparently unused — was then deleted.
- **The conclusion drawn at the time**: "authentication key unknown, RPMB
  permanently unusable". **That conclusion was wrong.**
- **The actual outcome**: the key was recovered from **freed blocks** on the SD
  card and confirmed cryptographically (`Read Write Counter` with HMAC and
  nonce-echo verification: recovered key 3/3, a one-bit variant and fully random
  keys 0/8). **RPMB is usable**; the rollback anchor can land.
- **Blast radius**: RPMB only, throughout. Boot lives on the SD card
  (`mmcblk1`); PL, network, OP-TEE and the demo path were unaffected — the board
  booted fully after the incident and the nine-section demo ran green.
- **The recovery was luck, not process**: the block holding those 65 bytes
  happened not to have been reused yet. At a different moment it would have been
  overwritten and this eMMC's RPMB would have been gone for good. **The incident
  therefore stands as a lesson and is not downgraded because it ended well.**

### How the rule is enforced (not just written down)

`provision` in `board/src/rpmb_tool.c` is **not compiled in** unless
`RPMB_ALLOW_PROVISION` is defined; running it only prints this rule and exits.
Provisioning a genuinely **new** board requires editing the source and
rebuilding (`-DRPMB_ALLOW_PROVISION=1`) — making it a **recorded decision**
rather than a command anyone can type.

Compile time rather than a runtime flag, for a direct reason: **a runtime flag
does not stop the "I know what I'm doing" moment**, and that moment is exactly
when this class of incident happens.

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
the whitelist is too wide but that **the SiP does not authenticate its caller**:
EL3 cannot tell "the daemon is calling" from "another
root process is calling". As long as root can issue an SMC, root can do anything
the daemon can; narrowing the whitelist only locks the daemon out as well.

| Approach | What it buys | Why not in this version |
|---|---|---|
| Operation-granularity SiP ("decapsulate this ciphertext with handle H" rather than "write this word to this address") | root can no longer assemble arbitrary register sequences | It moves the whole hardware sequence into EL3, **where there is no exception handling** — one refused bus access or one poll that never completes wedges a core, and the board can only be power-cycled. We measured exactly that with a single write. And it still does not authenticate the caller: root can invoke those operations too |
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
| **A key-export path still exists in the RTL** | `MODE.DK_TO_SLOT` keeps `dk` on-chip and `DK_FROM_SLOT` decapsulates by slot, which is what the daemon uses; ML-DSA's `sk` works the same way. The **non-vault** path remains, because ACVP verification has to read the private key out. In the delivery posture it is closed by a one-way latch — a latch, not a fuse, so it is re-opened by a power cycle |
| **Device binding is off by default** | The derivation root can be bound to this chip's Device DNA (`src/crypto/kdr_dna.c`), read through a **read-only** window in the BL31 whitelist (`0xFFCA0050-5C`); reading it directly from EL1 is a bus error. `PQCHSM_KDR=device-dna` switches to it. Off by default because changing the root makes an existing keystore unopenable with symptoms identical to tampering. ⚠️ **DNA is not a secret** — anyone with JTAG reads it — so the provider reports `device_bound=1`, `hardware_backed=0`: anti-cloning, not confidentiality |
| **No secure boot** | The PL configuration is loaded at runtime, not from a signed golden image, and on this board it cannot be otherwise — see [Boot and the trust root](#boot-and-the-trust-root) |
| **No module integrity check** | Nothing verifies the module image before use |
| **Secure memory is fenced against reads only** | BL31 poisons OP-TEE's DDR carve-out at boot, so the normal world cannot read it. It can still ask the hardware to compute — see [Memory isolation on DDR](#memory-isolation-on-ddr-xmpu) |
| **No physical security** | Logical enforcement only. The `tamper` input exists in RTL and zeroizes the vault; nothing is wired to it |
| **Conditional self-tests missing** | No pairwise consistency test on generated key pairs, no continuous RNG test |
| **No SM2** | SM4 and SM3 are implemented; the SM2 asymmetric algorithm is not |
| **No algorithm certificates** | ACVP vectors are run locally. That is evidence of correctness, not a certificate |
| **Single-writer audit log** | The audit module does not lock the file |
| **Unkeyed Shamir share checksums** | They detect corruption, not tampering |
| **Keystore rollback protection only stops swapping one file** | The header carries an `epoch` incremented on every write (covered by the MAC); where the anchor lives is chosen by a provider (`pqchsm/rbanchor.h`), and loading a file older than the anchor is **refused**. The default provider keeps the anchor in a sibling `<keystore>.epoch` file, which **is not real anti-rollback**: an attacker who rolls both files back consistently still gets through (`test_keystore` pins this weakness with a dedicated case). Real anti-rollback needs a monotonic value the attacker cannot rewind — see [Rollback anchor](#rollback-anchor) |
| **SO PIN lockout not enforced** | Failures are counted but the slot is not locked, because locking it would brick the device; recovery is by M-of-N |
| **`CKM_HASH_ML_DSA_*` not implemented** | PKCS#11 multi-part signing buffers the whole message instead of streaming a digest |
| **Host RNG is OpenSSL** | The PL entropy source is inside the boundary, but the host software still seeds from the operating system through OpenSSL |
| **Hold-timing margin is thin** | Effective hold margin is 0.110 ns after explicit management. A hold violation cannot be fixed by slowing the clock, so this number must be re-checked whenever the RTL changes; the implementation flow aborts below a 0.050 ns floor |

**Exactly one of these is permanently excluded rather than merely unfinished:**
eFUSE burning is behind this project's red line, which puts a replacement-resistant
trust root (`ENC_ONLY` / `RSA_EN`) out of reach for good. The rest are work, hardware
or bench time.

A FIPS 140-3 / GM/T 0028 style security policy draft, with its own gap list, is
in [reference/security-policy.md](reference/security-policy.md).

---

## Boot and the trust root

**There is no secure boot on this board, and the conclusion is settled.** The PL
configuration is loaded at runtime, not from a signed golden image. What follows is
why, because "not implemented yet" and "cannot be implemented here" are different
claims and only the second one is true.

### Three mechanisms that are routinely conflated

On ZynqMP these are separate, and treating them as one produces an overstated claim:

| Mechanism | What it buys | Where the root lives |
|---|---|---|
| **BBRAM** | AES-256 **confidentiality and integrity** of the boot image | Battery-backed RAM, rewritable, JTAG-programmable |
| **RSA authentication** | Resistance to **replacement** | eFUSE: `PPK*_HASH` plus `RSA_EN` |
| **`ENC_ONLY`** | Forces encrypted boot | eFUSE |

Without eFUSEs the BootROM enforces neither authentication nor encryption: an
attacker substitutes an image declared as neither, and it boots. **So encryption
alone is never a replacement-resistant trust root** — and eFUSE burning is this
project's permanent red line (see above). That ceiling holds regardless of which of
the paths below works.

### BBRAM: blocked by board hardware

Programming the BBRAM key latches `BBRAM_STS.PGM_MODE` (`0xFFCD0000` bit 0) at 1, and
while it is set the CSU will not hand the key to the BootROM. Neither XilSKey's own
`PrgrmDisable()`, nor a JTAG write to `0xFFCD0008`, nor `rst -system`, nor a PMU
system-scope reset clears it. A power-on reset does clear it — but this board has **no
cell on `VCC_BATT`**, so the same POR clears the key along with the latch. The two
requirements are mutually exclusive on this hardware.

This is not merely an obstacle to route around. BBRAM is battery-backed by design, so
even on a board with a battery a dead cell means an image that will not boot — which
is disqualifying for a form that must be ready at power-on.

### PUF black key: rejected before the PUF is reached

PUF boot-header mode does **not** require eFUSEs (`bootgen -bif_help fsbl_config`
defines `bh_auth_enable` as RSA authentication *excluding* the PPK-hash and SPK-ID
check), so it is admissible under this project's rules. Three defects in the image
were found and corrected — all worth knowing, because each has an authoritative source
and none is guessable:

| Defect | Authority |
|---|---|
| bif missing `puf4kmode`; bootgen **defaults to 12K** while registration was 4K | `bootgen -bif_help fsbl_config`; `xilskey_puf_registration.h` `XSK_PUF_MODE4K` |
| bif missing `shutter=0x0100005E` (bootgen's default `0x01000020` is wrong here) | `xilskey_eps_zynqmp_puf.h` `XSK_ZYNQMP_PUF_SHUTTER_VALUE` |
| Last helper-data word is `AUX << 4`, not `AUX` | `xilskey_eps_zynqmp_puf.c`: `Aux = (PufStatus & AUX_MASK) >> 4`, but `SyndromeData[385] = (PufStatus & AUX_MASK) << 4` |

With all three corrected and tested the valid way — a cold boot after a real POR — the
BootROM still rejects the image, and JTAG reads of the CSU show it happens **before
the PUF is ever consulted**: `PUF_CMD = 0`, `KEY_RDY = 0`, and OCM never loaded.

> Note on method: encrypted boot **cannot** be tested by a warm reboot. The secure
> state is latched by POR, so a warm reboot into an encrypted image is refused with
> `0x53` no matter what the image contains. A software POR is available on this part
> (the PMU error logic can be configured to generate one — `board/scripts/jtag_por.tcl`),
> which is what made the valid test possible.

### What does work, and why it is not enough

RSA authenticated boot works with **zero eFUSEs burned**, verified on the board. It is
not a trust root: `bh_auth_enable` is *defined* as skipping the eFUSE PPK hash check,
so the key it authenticates against travels in the image an attacker would replace.

**Net position**: confidentiality and replacement-resistance for the boot image are
both out of reach here, the first for board-hardware reasons and the second because
the only mechanism for it is behind a red line.

## Memory isolation on DDR (XMPU)

OP-TEE's secure carve-out at `0x6000_0000` was readable from normal-world userspace:
`devmem 0x60000000` returned `0xAA0003F3`, an AArch64 instruction from OP-TEE's own
code. The vendor boot chain configures no XMPU_DDR regions at all.

**The XMPU does not gate transactions.** XAPP1320 v3.0 p. 10 is explicit: on an
illegal access the XMPU asserts `AxUser[10]` and the transaction is still passed to
the memory controller — "the transaction is gated by the end point, not the XMPU
itself". Configuring regions therefore bought detection and nothing else, which is
exactly what was measured: violations latched, data still returned.

BL31 now writes `POISON.ATTRIB` (bit 20) on all six XMPU_DDR instances from EL3, at
boot, in the default form. **Only `POISON` is touched, never `CTRL`**, for two reasons
that both bite:

- `CTRL.DEFRDALWD` / `DEFWRALWD` govern accesses matching *no* region, so clearing
  them denies all of DDR;
- `CTRL.POISONCFG` selects poison-*by-address*, redirecting to `POISON.BASE` — and
  `BASE = 0`, so one illegal write would land at physical address 0.

Measured on the board, active at power-on with no manual step:

```
devmem 0x60000000 → Bus error      OP-TEE core carve-out, fenced
devmem 0x10000000 → 0xEDFE0DD0     outside the region, unaffected
devmem 0x70000000 → unaffected     OP-TEE shared memory, deliberately not fenced
```

Clearing poison and re-reading the same address returns the instruction again — the
same-address comparison is what proves the hole was real. Evidence:
`board/logs/RESULT_xmpu_persist.txt`.

⚠️ **This blocks reads, not use.** Root can still ask the hardware to compute, because
the SiP does not check its caller; see the residual-capability entry in the threat
model. The shared-memory segment is meant to be readable from both sides.


## The remote port

The daemon's TCP front end was once plaintext, authenticated by a static pre-shared
token: capturing one packet on the segment yielded the credential permanently, the
session had no integrity, and the auth frame was replayable.

It now runs inside **TLS 1.3 with certificates on both sides**
(`SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT`), the version pinned at both ends
so there is nothing to downgrade to. Identity moves from "knows a string" to "holds a
private key", which stops any host on the segment, any observer, any modifier, and any
replay.

**What it is still not** is access control:

- No privilege separation and no per-operation authorisation — passing mTLS grants
  every operation.
- The ACL is CN-granular (`pki/hsm_acl`, one CN per line). **An absent file means any
  certificate signed by this CA is accepted** — a deliberate default, explained at the
  declaration in `service/pqcs_tls.h`.
- No revocation. No CRL, no OCSP; a leaked client certificate means re-issuing the CA.
- Rate limiting is a per-IP leaky bucket (8 failed handshakes / 60 s). It addresses
  availability — a single-threaded daemon pinned by handshake spam — not
  confidentiality.

**The PKI is prototype grade**: the CA private key is a 0600 file on the operator's
machine, with no hardware protection, no intermediate CA and no revocation. It is
enough to replace a shared token with a private key; it is not a PKI in the
institutional sense.

**The local UNIX socket is deliberately not wrapped in TLS.** It is 0600 and
host-local, so reaching it already means root; encrypting it stops nobody and adds a
certificate for on-board tools to manage.

**The clock is a precondition.** Certificates carry validity dates and this board's
clock can fall behind `notBefore`. Under TLS 1.3 the rejection arrives *after* the
client considers the handshake complete, so the symptom is garbled device info rather
than an authentication error. Three mitigations: `demo_remote.sh --provision` syncs the
clock and writes the RTC, `hsm-boot.sh` refuses to leave the clock older than the
credentials on the SD card, and the client forces one `OP_PING` round trip at open so
such a rejection surfaces immediately.

Regression: `tools/tls_regress.sh` (ctest `tls_regress`) — one positive case and three
negatives (certificate from a different CA, no client certificate, CN not in the ACL).
**The negatives are the point**; "it connects" only shows the configuration is not
broken.

## Rollback anchor

`include/pqchsm/rbanchor.h` makes "where the monotonic epoch lives" a provider:

| Provider | Anchor | Real anti-rollback? |
|---|---|---|
| `file` (default) | sibling `<keystore>.epoch` | **No** — an attacker who rolls both files back consistently still gets through |
| `rpmb` | the eMMC RPMB **write counter** | Yes, under the assumption below |

**Why the counter and not a value stored in RPMB**: the RPMB data area can be rewritten
at will by whoever holds the authentication key, including to a smaller value. This
board has no secret hardware root, so the key can only live in a file — "the attacker
has the key" is an assumption that must be made. Anchoring on the counter makes it
survivable: **it can be pushed forward, it cannot be pulled back**, so an old snapshot
never matches again.

The hardware is sufficient: this board's eMMC (`mmcblk0`, Q2J55L) reports
`EXT_CSD[168] RPMB_SIZE_MULT = 32` → 4 MB RPMB, `/dev/mmcblk0rpmb` is present, and the
kernel supports `MMC_IOC_MULTI_CMD`.

⚠️ **RPMB responses on this eMMC alternate with stale frames.** Every response is
validated on `req_resp` and the nonce echo, never on `result`, and I/O failure returns
a different code from "wrong key". This is not defensive coding for its own sake — see
the red lines at the top of this document for what happens without it.

If RPMB is unavailable on some other board, the alternatives are: a **TPM NV counter**
(no TPM and no usable header on this board), an **eFUSE counter** (permanently
excluded here), a **replacement eMMC**, or a **remote anchor service** — which trades
the problem for "that server must always be reachable", untrue for an offline HSM but
workable for a networked deployment.

## Build profile: DEV and PRODUCTION

The stub key-derivation root is a **public constant compiled into the binary** (the
literal itself reads `NOT SECRET`). Falling back to it silently meant the keystore's
wrapping key was public by default, with nothing in the code saying so.

There are now two explicit profiles (`cmake -DPQC_PROFILE=…`, default `DEV`):

| | DEV (default) | PRODUCTION |
|---|---|---|
| Stub root key | compiled in | **`#if`'d out entirely — not findable in the binary** |
| No provider installed | falls back to the stub | provider is NULL, every derivation fails |
| Startup gate | proceeds, with an explicit warning | requires `hardware_backed`, else **refuses to start** |

`tools/check_profile.sh` (ctest `profile_no_stub_kdr`) scans the PRODUCTION object file
for that literal **and carries a null control** — it must still be findable in DEV,
otherwise renaming the literal would make the check pass forever without testing
anything.

⚠️ **This board cannot reach PRODUCTION.** There is no secret hardware root (eFUSE
excluded, BBRAM has no battery, the PUF black key never worked), and the `device-dna`
provider is `device_bound=1` but `hardware_backed=0` — the DNA differs per die, so
anti-cloning holds, but it is not a secret. A PRODUCTION build therefore refuses to
start here, which is **what the switch is meant to express**, not a defect. Demos and
regressions run DEV.
