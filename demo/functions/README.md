**English** · [中文](README.zh-CN.md)

# Crypto-module function tour

Walks through the management functions of the host-side crypto module (`pqchsmd`): algorithm support, entropy source, slot management, key management, backup and restore (M-of-N), and secure storage.

```bash
cmake -S . -B build && cmake --build build -j
./demo/functions/run.sh
```

The script starts its own daemon with a temporary keystore and removes both on exit. It needs no board and does not touch any existing keystore. It uses `cc` (to build `mechs` and `roundtrip`) and `python3` (to flip a bit in section 6).

Environment: `BUILD=` (default `build`), `PORT=` (default 19099), `SO_PIN=`, `USER_PIN=`.

## Which module this is

The repository has two modules with different feature sets:

| | Board: `pqchsm_fpgad` | Host: `pqchsmd` (this demo) |
|---|---|---|
| Slot management | None. Keys belong to the OP-TEE TA and are handed out as handles and wrapped blobs. Symmetric keys are loaded by the TA into the PL's symmetric key registers (slots 0 to 6 for business keys, 7 for public KAT keys) | State machine, labels, policy |
| PIN, session, role | None | SO and User, login, failure counting |
| Keystore | None with a PIN. The `hsmctl` / `hsm_httpd` keystore directory holds only TA-wrapped blobs | AES-GCM wrapping, KEK derived from the KDR, whole-file MAC, epoch anti-rollback |
| Backup and restore | `hsmctl backup` / `restore`: one bundle of the whole keystore with an SM3 per entry, no M-of-N | M-of-N Shamir shares |
| Cryptographic operations | In the PL | liboqs, or forwarded to the board |

Slot, backup and storage management in this demo are therefore the host side. The board side is in [demo/remote/](../remote/) and [board/demo/](../../board/demo/README.zh-CN.md).

The two can be combined. With `PQCHSM_BACKEND=sdfe`, ML-KEM key generation is done on the board: the TA keeps the seed as a wrapped blob and the PL expands it for each operation. The host slot then holds only the public key and a handle, and nothing is written to the host keystore for it.

## Sections

| Section | What it shows |
|---|---|
| 1. Algorithm support | `C_GetMechanismList` and `C_GetMechanismInfo`, asked of the module itself (5 mechanisms) |
| 2. Entropy source | Host-side randomness comes from OpenSSL. The board's TRNG (8 ring oscillators, H = 0.871 bit/sample, see [docs/SECURITY.md](../../docs/SECURITY.md)) is used only with `PQCHSM_HWRNG=sdfe` |
| 3. Slot management | List the four slots, the `UNINIT`, `EMPTY`, `LOADED` states, SO and User roles. Generating a key without logging in is refused (`not authorized`) |
| 4. Key management | Generate, export the public key, sign, destroy. There is no command to export a private key. Sizes are checked against FIPS 204 (ML-DSA-44: 1312-byte public key, 2420-byte signature) |
| 5. Backup and restore | Only slots with the `BACKUPABLE` policy are backed up. A 2-of-3 backup restores from shares 1 and 3 and from shares 1 and 2, and one share alone is refused |
| 6. Secure storage | Flipping one bit makes the keystore refuse to load and the file is not rewritten. An old snapshot is refused by the epoch anchor. `rotate-kek` re-wraps without changing keys |
| 7. Calling the algorithms | `roundtrip` runs `C_GenerateKeyPair`, `C_Sign`, `C_Verify`, then verifies again with one bit of the message flipped and with one bit of the signature flipped; both must fail. `board/demo/p11_hw_demo.c` runs the ML-KEM encapsulate, decapsulate and AES-GCM (KEM-DEM) path and checks that the private key cannot be read |

## Two things that are easy to misread

The `<slot>` argument of `backup` selects whose SO PIN authorises the backup. It does not select what is backed up: every slot with the `BACKUPABLE` policy is exported. The `pqchsm-admin` help text is ambiguous about this.

A key that is backed up and a key that stays on the board exclude each other in this keystore. A host-side backup contains the seed. A key generated on the board exists only as a TA-wrapped blob that only this board's TA can open, so the host keystore cannot export it. The board's own bundle (`hsmctl backup`) restores only onto the same board.

## Default key-derivation root

The default KDR is a public constant compiled into the binary (the daemon prints a `DEV` warning at start), so keystore confidentiality in this configuration is zero. A PRODUCTION build refuses to start without a hardware-derived root. See the build-profile section of [docs/SECURITY.md](../../docs/SECURITY.md).
