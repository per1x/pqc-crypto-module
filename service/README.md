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
| `sdf_demo.c` | A worked example application |

```bash
make                              # host build
make CROSS=aarch64-linux-gnu-     # for the board
```

Everything is statically linked, so the binaries can be copied to the board
without matching its libc.

## Why the daemon cannot be dropped

All three cores are stateful sequences — write registers, start, poll, read
result. Two processes interleaving on one core produce results that are mutually
misaligned but individually plausible. Serialisation needs a single point, and
that point has to remember sessions.

## Why `sdf_demo` links no crypto library

`sdf_demo` links **only** `libsdfe`. It has no algorithm implementation
available to it at all, so it cannot compute anything — every correct value it
prints came out of the FPGA. That is the point of the demo, and it should stay
that way.

The file also contains no hardware detail: no registers, no `/dev/mem`, no SMC.
Swap in a different crypto module that implements the same interface and not one
line of it changes.

## Private keys

| Key | Where | Leaves the hardware? |
|---|---|---|
| Symmetric (AES/SM4) | `key_vault` | **No** — the RTL has no bus-side read path |
| ML-KEM `dk` | Returned by `KeyGen`, held by the daemon | **Yes**, but never crosses the interface; the application only holds a handle |

The second row must not be shortened to "private keys never leave the
hardware". See [../docs/SECURITY.md](../docs/SECURITY.md#limitations).

Interface reference: [../docs/API.md](../docs/API.md).
