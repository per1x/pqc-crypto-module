# service/

The layer that lets an ordinary application use the FPGA cores through a
standard crypto-module interface, without knowing that registers exist.

```
application → libsdfe → pqchsm_fpgad → /dev/secmmio → EL3 SiP → AXI firewall → cores
```

| File | Role |
|---|---|
| `sdfe.h`, `libsdfe.c` | SDF-style client library. Stateless; several processes may each link it |
| `pqchsm_fpgad.c` | Daemon: sessions, handles, serialisation, ML-KEM private key custody |
| `wire.h` | The request/response encoding between the two |
| `pqcs_tls.h`, `pqcs_tls.c` | mTLS for the remote port: why, and what it does and does not buy |
| `sdf_demo.c` | A worked example application |
| `hsmctl.c` | Command-line tool: one command per business operation (see below) |

```bash
make                              # host build
make CROSS=aarch64-linux-gnu- \
     SYSROOT=<target sysroot> SSLROOT=<cross-built openssl>   # for the board
```

The remote port is **mTLS** (`pqcs_tls.c`), so both sides link OpenSSL.

⚠️ The board build is **not** fully static any more. Linking this prebuilt
OpenSSL 1.1.1d statically segfaults on the target *before `main`* — a constructor
of our own never gets to print, and it happens whenever `libssl.a` enters the
link. The board build therefore links OpenSSL dynamically, ships
`libssl.so.1.1` / `libcrypto.so.1.1` in `hsm/lib/` on the SD card, and pins the
lookup path with `-rpath`. The full reasoning is in the header of `Makefile`.

## Why the daemon cannot be dropped

All three cores are stateful sequences — write registers, start, poll, read
result. Two processes interleaving on one core produce results that are mutually
misaligned but individually plausible. Serialisation needs a single point, and
that point has to remember sessions.

## Why `sdf_demo` links no crypto library

`sdf_demo` links `libsdfe` and OpenSSL, and **OpenSSL is used for TLS transport
only** — there is no ML-KEM, ML-DSA, SM4 or SM3 implementation available to it at
all, so it cannot compute anything itself. Every correct value it prints came out
of the FPGA. That is the point of the demo, and it should stay that way.

The file also contains no hardware detail: no registers, no `/dev/mem`, no SMC.
Swap in a different crypto module that implements the same interface and not one
line of it changes.

## Private keys

| Key | Where | Leaves the hardware? |
|---|---|---|
| Symmetric (AES/SM4) | `key_vault` (authoritative copy: TA-wrapped blob held by the caller) | **No** — the RTL has no bus-side read path, and the daemon never sees plaintext |
| ML-KEM `dk` | Not stored anywhere in expanded form: the TA keeps the **seed** as a wrapped blob; per operation the TA stages the seed into the PL, which expands, computes and wipes (batch 2, stateless PL) | **No** — the daemon holds only the blob; the slot ABI and `-lock` latch were removed |
| ML-DSA `sk` | Same (seed `ξ` as a TA blob) | **No**, same |

Symmetric keys are generated and loaded by the TA (`SDFE_GenerateKey` /
`SDFE_LoadKey` / `SDFE_LoadKatKey`; wire ops `OP_SYMKEY_GEN`/`LOAD`/`KAT`, 15–17).
The daemon only carries the blob; slots 0..6 are product slots, slot 7 is
KAT-only. `SDFE_ImportKey` (plaintext, op 6) always returns `SDR_NOTSUPPORT`,
and the daemon wipes the symmetric vault at session end. Block encryption is
unchanged: it runs in the PL by slot number, with no SMC per block.

⚠️ The `dk` row used to read "returned by KeyGen, held by the daemon" (true before
`dk_to_slot`), and later "on-chip key vault slot + `-lock` latch" (true before
batch 2). A stale comment on a security claim is the most
expensive kind: a reader concludes the private key leaves the hardware when it
does not, or the reverse. See
[../docs/SECURITY.md](../docs/SECURITY.md#limitations).

Interface reference: [../docs/API.md](../docs/API.md).

## `hsmctl` — command reference

`hsmctl` runs on the board (local UNIX socket to `pqchsm_fpgad`). Like `sdf_demo` it
links only `libsdfe` and performs no cryptography itself. Build: `make hsmctl`.
The full Chinese manual is in the repository root `README.zh-CN.md`
(「命令行：hsmctl 使用手册」).

Keys live in a keystore directory, one `<name>.key` file per key
(`HSM_KEYSTORE`, default `./keystore`). A key file never holds a plaintext key:
symmetric keys are stored as the TA's KEK-wrapped blob, post-quantum keys as the
public key plus the TA-wrapped seed blob. Only the TA on this board can unwrap them.

| Command | Purpose |
|---|---|
| `info` | Device info and keystore location |
| `keygen <alg> <name>` | Generate a key: `sm4 aes128 aes256 mlkem512 mlkem768 mlkem1024 mldsa44 mldsa65 mldsa87` |
| `list` / `show <name>` | List the keystore / inspect one key file (public-key SM3 fingerprint, blob head) |
| `encrypt <name> <in> <out>` / `decrypt <name> <in> <out>` | Symmetric CBC + PKCS#7; IV from the PL TRNG; output = IV ‖ ciphertext |
| `sign <name> <file> <sig>` / `verify <name> <file> <sig>` | ML-DSA |
| `encap <name> <ct>` / `decap <name> <ct>` | ML-KEM; both print the shared secret |
| `hash <file>` / `random <n>` | SM3 / TRNG bytes |
| `backup <bundle>` / `restore <bundle>` | Keystore bundle with a per-entry SM3; restore verifies everything before writing anything |
| `destroy <name>` | Overwrite and delete a key file |

Exit status: `0` success, `1` error (including the TA refusing a tampered or
foreign blob), `2` a business "no" (signature does not verify, bundle checksum mismatch).

Each layer the daemon calls logs a tagged line once that call has succeeded —
`[TEE]` (TA), `[TEE→PL]` (secure path into the FPGA), `[PL]` (FPGA core, with
latency), `[NW]` (daemon only moves ciphertext blobs) — in `hsm/hsm-boot.log`.
The board demo `demo/run.sh` runs the full business flow with those traces;
`tools/demo_to_html.py` turns its output into a report
(sample: `docs/demo/business_demo.html`).

## `hsm_httpd` — JSON gateway

Same business code as `hsmctl` (`hsmops.c`), exposed as JSON over HTTP.
Two listeners: `127.0.0.1:8080` plain HTTP bound to loopback only (on-board
apps and the demo; same trust boundary as running `hsmctl`), and `:8443`
HTTPS with mutual TLS (client cert must chain to `hsm/pki/hsm_ca.crt`; no
listener if credentials are missing). Usage: `hsm_httpd [tls_port [local_port]]`,
defaults 8443 / 8080, `0` disables one.

Routes: `GET /v1/info`, `GET|POST /v1/keys`, `GET|DELETE /v1/keys/{name}`,
`POST /v1/keys/{name}/{encrypt,decrypt,sign,verify,encapsulate,decapsulate}`,
`POST /v1/sm3`, `POST /v1/random`. Binary fields are base64 (`*_b64`), digests
and shared secrets hex. Add `?trace=1` to get the per-layer log lines for that
request. Part 8 of `demo/run.sh` calls every route on the board via `nc`.
