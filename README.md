**English** · [中文](README.zh-CN.md)

# pqc-crypto-module

A post-quantum + SM-series **hardware security module prototype** running in the
programmable logic of a Xilinx Zynq UltraScale+ **XCZU3EG**. The cryptographic
cores, the noise source, the key vault and the access boundary all live in FPGA
fabric; Linux on the ARM cores is only the side that issues commands.

Every number in this repository comes from **the real device**, not simulation.

> **Status: research prototype.** Not certified, not hardened, not for
> production. See [Status and limitations](#status-and-limitations) — the gaps
> are listed, not hidden.

---

## What it does

| | |
|---|---|
| **Post-quantum KEM** | ML-KEM-512 / 768 / 1024 (FIPS 203), full KeyGen / Encaps / Decaps in RTL — byte-exact against NIST ACVP vectors **on silicon** |
| **Symmetric & SM-series** | AES-128/256, SM4, SM3 — checked against FIPS 197, GB/T 32907, GB/T 32905 |
| **True random source** | 8-ring-oscillator entropy source, SP 800-90B health tests (RCT/APT), SHA-3 sponge conditioning. Measured min-entropy **H = 0.871 bit/sample** |
| **Hardware key vault** | Symmetric keys enter over the bus and leave only over a private wire to the cipher cores. There is **no read path in the RTL** |
| **Security boundary** | AXI firewall gating on `AxPROT[1]`, proven in both directions on the board: the secure world reads a `SECURE_ONLY=1` core, the normal world is refused at the bus |
| **Standard front end** | SDF-style (GM/T 0018) C library and a PKCS#11 v3.2 module, so an application never sees a register |

Fits in **half the device**: 35,659 LUT (50.5 %), 140 DSP, 15.5 BRAM,
WNS +3.504 ns @ 75 MHz.

## Architecture

```
   ┌──────────────────────── PS · Cortex-A53 ────────────────────────┐
   │  application ──▶ libsdfe (SDF-style)  ──▶ pqchsm_fpgad          │
   │                                              │                  │
   │  ······················· normal world ·······│················  │
   │                                       /dev/secmmio  →  EL3 SiP  │
   └──────────────────────────────────────────────│──────────────────┘
                       M_AXI_HPM0_LPD  (AXI4, AxPROT[1] = security bit)
                                                  ▼
   ┌──────────────────────── PL · FPGA fabric ───────────────────────┐
   │              axi4lite_xbar  (full decode, one address per reg)  │
   │   ┌──────┬──────────┬──────────┬──────────┬─────────┬────────┐  │
   │   │ slot0│  slot1   │  slot2   │  slot3   │  slot4  │ slot5  │  │
   │   │ trng │key_vault │   sym    │  mlkem   │ canary  │  fan   │  │
   │   │      │          │AES/SM4/  │ 512/768/ │ same as │ observe│  │
   │   │      │          │   SM3    │   1024   │ slot 1  │  only  │  │
   │   │      SECURE_ONLY=1 (default build)              │ =0     │  │
   │   └──────┴────┬─────┴────▲─────┴──────────┴─────────┴────────┘  │
   │               └──────────┘  use_key: private wire, not the bus  │
   │      every slot sits behind axi4lite_firewall (AxPROT gate)     │
   └─────────────────────────────────────────────────────────────────┘
```

Each slot is 64 KB at `0x8000_0000 + slot × 0x1_0000`.

**In the default build all four functional slaves are `SECURE_ONLY=1`** — the
normal world cannot reach any of them, and the whole KAT suite is driven from
the secure world through the BL31 SiP. Slot 4 remains a second key-vault
instance; under this configuration it is a like-for-like control that shows a
refusal is the gate's doing and not a broken core. The fan (slot 5) stays 0: it
is not inside the cryptographic boundary and should not be.

A second configuration exists for development — `PQC_DEV_OPEN=1` sets the
functional slaves to `SECURE_ONLY=0` so Linux can drive them directly, and its
product is named `zu3eg_hsm_dev.bit`. Same RTL, one parameter apart.

> ⚠️ **A refused access reads back 0; it does not raise an error.** The
> firewall and the address decoder are RAZ/WI, so no user-space program can
> take the board down with a bad address — deliberate, and verified on silicon
> (36,000 refused accesses, board still up). The flip side is that a mistyped
> address is silent; check `VERSION`
> (`0x0001_0000` on every core) rather than looking for an error code. Details
> in [docs/REGISTERS.md](docs/REGISTERS.md).

Full detail: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) ·
register-level contract: [docs/REGISTERS.md](docs/REGISTERS.md).

## Quick start

No board is needed for anything in this section.

```bash
git clone https://github.com/per1x/pqc-crypto-module
cd pqc-crypto-module

python3 -m venv .venv-rtl && ./.venv-rtl/bin/pip install cocotb
brew install icarus-verilog verilator      # or your distro's packages

./tools/rtl_sim.sh          # 200 cocotb tests against the RTL
./tools/rtl_lint.sh         # Verilator -Wall + Icarus, 70 modules, zero warnings
./tools/rtl_synth_check.sh  # Yosys synthesisability
```

To build the host software and its PKCS#11 module (CMake ≥ 3.20, OpenSSL 3,
liboqs):

```bash
./tools/fetch_vectors.sh && cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

Bitstream (Vivado 2020.1, ~35 min, target `xazu3eg-sfvc784-1-i`):

```bash
vivado -mode batch -source hardware/syn/impl_bitstream.tcl
```

### Calling it like a crypto module

`service/` gives an application the interface a real HSM exposes. The demo links
**only** `libsdfe` — no crypto library at all — so any correct answer it prints
can only have come from the FPGA.

```c
#include "sdfe.h"

SDFE_HANDLE dev, ses;
uint8_t ek[1600], ct[1600], ss[32];
uint32_t ek_len = sizeof ek, ct_len = sizeof ct, ss_len = sizeof ss, kh;

SDFE_OpenDevice(&dev);
SDFE_OpenSession(dev, &ses);

SDFE_GenerateRandom(ses, 32, ss);                    /* ring-oscillator TRNG  */
SDFE_GenerateKeyPair_MLKEM(ses, SDFE_MLKEM_768,      /* dk never reaches here */
                           ek, &ek_len, &kh);
SDFE_Encapsulate_MLKEM(ses, SDFE_MLKEM_768, ek, ek_len,
                       ss, &ss_len, ct, &ct_len);
SDFE_Decapsulate_MLKEM(ses, kh, ct, ct_len, ss, &ss_len);   /* by handle */

SDFE_ImportKey(ses, /*slot*/ 3, key, 16);            /* into the key vault —  */
SDFE_Encrypt(ses, SDFE_ALG_SM4, 3, pt, out);         /* unreadable afterwards */
```

Build and run: `make -C service && ./service/sdf_demo`.
Interface reference: [docs/API.md](docs/API.md).

## Evidence

Measured on the device, in the configuration described in
[docs/SECURITY.md](docs/SECURITY.md).

| Check | Result |
|---|---|
| ML-KEM 512/768/1024 vs NIST ACVP, on silicon | 20 / 20 byte-exact |
| Board self-test (symmetric, SM, boundary, AxPROT, TRNG) | 24 / 24 |
| Key vault counter-proof — 256 bytes scanned on each of two slaves | key words appear **0** times; ciphertext correct |
| AxPROT gate, both directions *(measured on the development bitstream, where a `SECURE_ONLY=0` control exists; taken before the RAZ/WI change, so refusal was logged as DECERR)* | EL3 reads `SECURE_ONLY=1`; EL1-NS refused at the same address; same EL1-NS reads a `SECURE_ONLY=0` core successfully |
| Default (shipping) bitstream — normal world reachability | 0 of 5 cores readable; 6 / 6 refused |
| TRNG min-entropy, 1,048,576 pre-conditioning samples | H = 0.871234 bit/sample → RCT 47, APT 672 |
| Decaps timing, valid vs implicit-reject, 200 runs each | median difference 0.000 % |
| ML-KEM-512 throughput @ 75 MHz | 924 / 1339 / 1018 ops/s (KeyGen / Encaps / Decaps) |
| cocotb regression · Verilator lint · Yosys | 200 tests · 70 modules, 0 warnings · all synthesise |

Method and raw logs: [docs/TESTING.md](docs/TESTING.md); on-board captures are
kept verbatim under [board/logs/](board/logs/).

## Status and limitations

Accurate statements about what this does **not** do.

- **ML-KEM private keys leave the hardware.** `KeyGen` returns `ek ‖ dk` over
  AXI, because checking against ACVP vectors requires it. The daemon keeps `dk`
  and hands the application a handle, so it does not leave the *interface* — but
  "the private key never leaves the hardware" is not yet true, and is not
  claimed. Symmetric keys in the key vault are a different case: those genuinely
  have no read path.
- **root can still drive the hardware.** The EL3 SiP exposes whitelisted MMIO
  reads and writes, with the operation sequence assembled in the normal world.
  The normal world cannot *read* key material; it can still load and use key
  vault slots. Closing that needs an operation-granularity SiP.
- **The key derivation root is a stub.** `src/crypto/kdr.c` holds a fixed
  constant. A real device takes it from eFUSE, BBRAM or a PUF; on this board
  eFUSE is irreversible with a single board available, and BBRAM needs JTAG.
  Device binding is therefore not real.
- **No SM2 core.** SM4 and SM3 are present; the SM2 asymmetric algorithm is not.
- **This is a security prototype, not a fast one.** Throughput figures include
  per-byte software AXI traffic and are published for the 1 : 1.5 : 2.1 ratio
  across parameter sets, not as a performance claim.
- **Power and EM side channels are out of scope, deliberately.** Constant-time
  work covers timing only. Masking cannot be validated without a side-channel
  bench, and shipping unvalidated masking is worse than shipping none.
- **ML-DSA is operators only.** Thirteen modules verified against the reference
  model, not chained into whole KeyGen/Sign/Verify cores.
- **Two items need JTAG**: persisting the PL configuration into the golden
  `BOOT.BIN`, and BBRAM-backed secure boot.
- **PS-side XMPU/XPPU does not apply.** UG1085 settles it: no PS protection unit
  covers the `0x8000_0000` PL window. The firewall in the PL is the only
  enforcement point on this path — which is why the address decode is one-to-one
  with no mirrors.

## Repository layout

```
hardware/rtl/       Verilog-2001 crypto cores: mlkem/ mldsa/ keccak/ sym/ trng/ bus/ board/
hardware/platform/  Non-crypto fabric logic (fan control) — same bitstream, no shared signals
hardware/tb/        cocotb testbenches, simulation tops, lint-only vendor stubs
hardware/model/     Python reference model, independent oracles, vector export
hardware/syn/       Vivado out-of-context synthesis and the RTL-to-bitstream flow
service/            SDF-style client library, daemon, and a hardware-only demo
boot/atf/           BL31 patches: the EL3 SiP that gives the secure world a path to the PL
board/              On-board programs, harness, kernel modules, and raw result logs
include/ src/ cli/  Host software: keystore, slots, backup, audit, PKCS#11 front end
tee/                OP-TEE trusted application (separate line of work)
tests/              Host-software unit, integration, KAT and fuzz targets
tools/              Regression scripts, SP 800-90B estimators, static analysers
docs/               Architecture, API, security model, testing, register maps
```

## Documentation

| | |
|---|---|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layering, address map, clocking, key hierarchy, hardware seam |
| [API.md](docs/API.md) | SDF-style interface and PKCS#11 v3.2 front end |
| [SECURITY.md](docs/SECURITY.md) | Trust boundary, threat model, what is proven, what is not |
| [REGISTERS.md](docs/REGISTERS.md) | Register contract for every AXI slave |
| [TESTING.md](docs/TESTING.md) | What is tested, by what means, how to reproduce it |
| [USAGE.md](docs/USAGE.md) | Building, running, deploying, and driving the board |
| [reference/](docs/reference/) | Security policy draft, offline deployment, constant-time audit, port plan |

Chinese versions live beside each file as `*.zh-CN.md`. Architecture, API,
registers, security and testing are also published together as a single PDF —
[设计与验证参考](docs/reference/design-validation.zh-CN.pdf), 26 pages — built
from those same Markdown files by `./tools/pdf/build-pdf.sh`, so the two cannot
drift apart.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). To report a vulnerability, read
[SECURITY.md](SECURITY.md) first — but note that this is a prototype, and the
known gaps above are already known.

## License

[Apache-2.0](LICENSE) — chosen over MIT for its explicit patent grant and patent
retaliation clause; patents are a real exposure in this field and MIT is silent
on them.

`third_party/pkcs11-v3.2/` contains three OASIS headers, included unmodified
under their own terms.
