**English** · [中文](README.zh-CN.md)

# Crypto-module function tour

One command through the management surface a crypto module is expected to have:
algorithm support, entropy source, slot management, key management, backup and
restore (M-of-N threshold), and secure storage.

```bash
cmake -S . -B build && cmake --build build -j
./demo/functions/run.sh
```

Self-contained: temporary keystore, its own daemon, cleaned up on exit. **No board
required**, and it touches nothing you already have.

## ⚠️ Which crypto module this is

There are two of them in this repository, with completely different surfaces:

| | On the board: `pqchsm_fpgad` | On the host: `pqchsmd` (this demo) |
|---|---|---|
| Slot management | ❌ Its "slots" are PL key-vault 0-7 — hardware that holds symmetric keys | ✅ State machine, labels, policy |
| PIN / session / role | ❌ | ✅ SO / User, login, failure counting |
| Keystore | ❌ | ✅ AES-GCM wrapped, KEK from the KDR, whole-file MAC, epoch anti-rollback |
| Backup / restore | ❌ | ✅ M-of-N Shamir shares |
| Cryptographic operations | ✅ All in the FPGA | liboqs, or forwarded to the board |

**Slot, backup and storage management all live on the host side.** The board half is
in [demo/remote/](../remote/) and [board/demo/](../../board/demo/).

The two connect: with `PQCHSM_BACKEND=sdfe`, ML-KEM key generation runs on the FPGA
and the private key stays in the on-chip vault — so slots and backups are managed on
the host while the key material lives in hardware. That comes with a real trade-off,
below.

## What each section proves

| Section | The point |
|---|---|
| 1. Algorithm support | `C_GetMechanismList` + `C_GetMechanismInfo` — **ask the module itself**, not its documentation. Where they disagree, this wins |
| 2. Entropy source | The host side's randomness comes from OpenSSL, **not** the PL entropy source. The hardware source (8 ring oscillators, H = 0.871) is on the board |
| 3. Slot management | Enumerate every slot, the `UNINIT → EMPTY → LOADED` state machine, SO and User roles. **Negative: generating without logging in is refused** |
| 4. Key management | Generate / export public key / sign / destroy. **There is no "export private key" command — that is a structural fact, not an omission** |
| 5. Backup and restore | A policy bit decides what can be backed up; 2-of-3 threshold. **Negative: one share must fail**; a different pair still works, so it is a real threshold scheme |
| 6. Secure storage | **Negative A**: flip one bit → refuses to load **and does not rewrite the file**. **Negative B**: replay an old snapshot → refused by the epoch anchor |

The negatives are the point. A crypto module is defined by what it **refuses**, not by
what it computes.

## Two things that are easy to read backwards

**`backup`'s `<slot>` argument is not "which slot to back up"** — it is *whose SO PIN
authorises the backup*. What gets exported is every slot carrying the `BACKUPABLE`
policy. The `pqchsm-admin` help string is ambiguous here.

**"Backupable" and "stays in hardware" are mutually exclusive.** A backup carries the
seed, and a private key that has entered the FPGA's on-chip vault has no read path —
so it cannot be backed up. Recoverable ↔ non-exportable: pick one. That is not a
defect; it is a decision the operator has to make.

## Environment

`BUILD=` (default `build`), `PORT=` (default 19099), `SO_PIN=`, `USER_PIN=`.
