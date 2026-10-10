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
