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
| Slot management | ❌ No slot state machine. Keys are owned by the OP-TEE TA and handed out as handles plus wrapped blobs; "slots" 0-7 are PL key-vault entries the TA loads symmetric keys into | ✅ State machine, labels, policy |
| PIN / session / role | ❌ | ✅ SO / User, login, failure counting |
| Keystore | ❌ No PIN-protected keystore; the `hsmctl` / `hsm_httpd` keystore directory holds only TA-wrapped blobs (no plaintext key, readable only by this board's TA) | ✅ AES-GCM wrapped, KEK from the KDR, whole-file MAC, epoch anti-rollback |
| Backup / restore | `hsmctl backup` / `restore`: a whole-keystore bundle with a per-entry SM3, no M-of-N | ✅ M-of-N Shamir shares |
| Cryptographic operations | ✅ All in the FPGA | liboqs, or forwarded to the board |

**Slot, backup and storage management all live on the host side.** The board half is
in [demo/remote/](../remote/) and [board/demo/](../../board/demo/).

The two connect: with `PQCHSM_BACKEND=sdfe`, ML-KEM key generation goes to the board —
the TA holds the seed as a wrapped blob and the FPGA expands it per operation — so slots and
backups are managed on the host while the key material stays on the board. That comes
with a real trade-off, below.

## What each section proves

| Section | The point |
|---|---|
| 1. Algorithm support | `C_GetMechanismList` + `C_GetMechanismInfo` — **ask the module itself**, not its documentation. Where they disagree, this wins |
| 2. Entropy source | The host side's randomness comes from OpenSSL, **not** the PL entropy source. The hardware source (8 ring oscillators, H = 0.871) is on the board |
| 3. Slot management | Enumerate every slot, the `UNINIT → EMPTY → LOADED` state machine, SO and User roles. **Negative: generating without logging in is refused** |
| 4. Key management | Generate / export public key / sign / destroy. **There is no "export private key" command — that is a structural fact, not an omission** |
| 5. Backup and restore | A policy bit decides what can be backed up; 2-of-3 threshold. **Negative: one share must fail**; a different pair still works, so it is a real threshold scheme |
| 6. Secure storage | **Negative A**: flip one bit → refuses to load **and does not rewrite the file**. **Negative B**: replay an old snapshot → refused by the epoch anchor |
| 7. Calling the algorithms | Support is not use. The signature path runs `C_GenerateKeyPair → C_Sign → C_Verify`, then verifies again with **one bit flipped in the message, and again with one bit flipped in the signature — both must fail**. The KEM path runs encapsulate → decapsulate → AES-GCM (KEM-DEM), and shows the private-key export being refused |

The negatives are the point. A crypto module is defined by what it **refuses**, not by
what it computes.

## Two things that are easy to read backwards

**`backup`'s `<slot>` argument is not "which slot to back up"** — it is *whose SO PIN
authorises the backup*. What gets exported is every slot carrying the `BACKUPABLE`
policy. The `pqchsm-admin` help string is ambiguous here.

**"Backupable" and "stays on the board" are mutually exclusive in this keystore.** A
host-side backup carries the seed, and a key that lives on the board is held there as a
TA-wrapped blob that only this board's TA can open — the host keystore cannot export it.
Recoverable ↔ non-exportable: pick one. That is not a defect; it is a decision the
operator has to make. (The board's own bundle, `hsmctl backup`, restores only onto the
same board.)

## Environment

`BUILD=` (default `build`), `PORT=` (default 19099), `SO_PIN=`, `USER_PIN=`.
