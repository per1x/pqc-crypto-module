**English** · [中文](README.zh-CN.md)

# pqc-crypto-module

A post-quantum and SM-series cryptographic module prototype on a Xilinx Zynq
UltraScale+ XCZU3EG (ALINX AXU3EGB board). Keys are managed by an OP-TEE trusted
application (TA) in the secure world on the ARM cores; cryptographic operations and
the true random number generator run in the FPGA fabric (PL). Linux in the normal
world forwards requests and stores the wrapped key files.

This is a research prototype. It has not been certified or hardened and must not be
used in production. Known problems are listed under [Limitations](#limitations).

## Features

| Area | What is there |
|---|---|
| Post-quantum KEM | ML-KEM-512 / 768 / 1024 (FIPS 203). KeyGen / Encaps / Decaps in the PL, byte-exact against NIST ACVP vectors on the board |
| Post-quantum signatures | ML-DSA-44 / 65 / 87 (FIPS 204). KeyGen / Sign / Verify in the PL; one bitstream, parameter set chosen at run time; matches ACVP vectors on the board |
| Symmetric and SM | AES-128 / 256, SM4, SM3, checked against FIPS 197, GB/T 32907 and GB/T 32905 vectors |
| True random numbers | 8 ring oscillators in the PL, SP 800-90B health tests (RCT / APT), SHA-3 conditioning; measured min-entropy 0.871 bit/sample |
| Key management | Generation, wrapping and unwrapping in the OP-TEE TA; only KEK-wrapped blobs (AES-256-GCM) leave it |
| Interfaces | SDF-style (GM/T 0018) C library, PKCS#11 v3.2 module, `hsmctl` CLI, HTTP (JSON) gateway |

## Who does what

| Where | Does | Does not |
|---|---|---|
| OP-TEE TA (secure world, S-EL0) | Generates keys (entropy from the OP-TEE PRNG mixed with the PL TRNG), wraps and unwraps them under the KEK, and writes key material into the PL through PTA → BL31 SiP when a key is used | No block cipher or post-quantum computation |
| BL31 (EL3) | SiP calls: whitelisted register access, seed and symmetric-key loading, TRNG words | Holds no keys |
| PL (FPGA) | All cryptographic computation and the TRNG. A symmetric key sits in the key registers for one session and is cleared at session end; a post-quantum private key is expanded from its seed for each operation and cleared by hardware afterwards | Keeps no long-term keys; the key registers have no read path |
| Normal-world daemon `pqchsm_fpgad` | Sessions, serialised hardware access, issuing operations through `/dev/secmmio`, passing key blobs between the application and the TA | Never sees a plaintext key and cannot unwrap a blob |
| Applications (`libsdfe` / `hsmctl` / `hsm_httpd`) | Call the interfaces; keys are stored as blob files on the normal filesystem | No cryptographic computation |

The KEK exists only inside the TA, so a key file copied elsewhere is useless; only the
board that created it can unwrap it.

## Architecture

```
 ┌──────────────────────────── PS · Cortex-A53 ────────────────────────────┐
 │ application ─▶ libsdfe / hsmctl / hsm_httpd ─▶ pqchsm_fpgad (daemon)    │
 │                                   op sequences │          │ key blobs   │
 │ ···························· normal world ·····│··········│·············│
 │                                     /dev/secmmio     OP-TEE TA (S-EL0)  │
 │                                                │     gen / wrap / unwrap│
 │                                                │     PTA (S-EL1)        │
 │                                                ▼          ▼             │
 │              BL31 SiP (EL3): whitelisted MMIO · seed / sym-key loading  │
 └────────────────────────────────────────┬────────────────────────────────┘
                 M_AXI_HPM0_LPD  (AXI4, AxPROT[1] = security bit)
                                          ▼
 ┌──────────────────────────── PL · FPGA fabric ───────────────────────────┐
 │                axi4lite_xbar  (full decode, one address per reg)        │
 │ ┌───────┬──────────┬─────────┬──────────┬─────────┬─────────┬─────────┐ │
 │ │ slot0 │  slot1   │  slot2  │  slot3   │  slot4  │  slot5  │  slot6  │ │
 │ │ trng  │ sym key  │   sym   │  mlkem   │ canary  │   fan   │  mldsa  │ │
 │ │       │ registers│AES/SM4/ │ 512/768/ │ same as │ observe │44/65/87 │ │
 │ │       │          │   SM3   │   1024   │ slot 1  │  only   │         │ │
 │ └───────┴────┬─────┴────▲────┴──────────┴─────────┴─────────┴─────────┘ │
 │              └──────────┘  key goes to the cipher over a direct wire    │
 │  slots 0-4 and 6 sit behind axi4lite_firewall (AxPROT gate);            │
 │  the fan port (slot 5) is outside the boundary, no firewall             │
 └─────────────────────────────────────────────────────────────────────────┘
```

Each slot is 64 KB at `0x8000_0000 + slot × 0x1_0000`.

| Slot | Module | Notes |
|---|---|---|
| 0 | `trng_axi` | True random numbers |
| 1 | `key_vault_axi` | Symmetric key registers, 8 entries (0–6 for applications, 7 only for public KAT keys). The load port accepts secure transactions only; normal-world writes are dropped and counted in `LOAD_REJ`. No read path to the bus; the key reaches slot 2 over a direct wire |
| 2 | `sym_axi` | AES / SM4 / SM3 |
| 3 | `mlkem_axi` | ML-KEM, all three parameter sets |
| 4 | control instance | Same module as slot 1 with `SECURE_ONLY` fixed at 1; shows that a read of 0 comes from the firewall, not a broken core |
| 5 | `fan_ctrl_axi` | Fan status and override; outside the cryptographic boundary, no firewall |
| 6 | `mldsa_axi` | ML-DSA, all three parameter sets |

In the default bitstream the five functional slaves (slots 0, 1, 2, 3, 6) are
`SECURE_ONLY=1`; a direct normal-world access reads 0. The daemon reaches the cores
through `/dev/secmmio`, which hands each access to BL31 to issue from EL3 against a
whitelist. The whitelist excludes the slot 1 load port and the post-quantum seed
ports; only the TA can use those, through the PTA.

`PQC_DEV_OPEN=1` builds the development bitstream `zu3eg_hsm_dev.bit`, with the
functional slaves at `SECURE_ONLY=0` so Linux can drive them directly. Both bitstreams
come from the same RTL. The symmetric-key load port and the seed ports stay
secure-only in the development bitstream too. At present `hsm-boot.sh` loads the
development bitstream at power-on; the default (`SECURE_ONLY=1`) bitstream is used to
verify the boundary and is loaded by hand.

A refused access reads 0, a refused write is dropped, and the bus still answers OKAY
(RAZ/WI). No user-space program can hang the board with a bad address (tested on the
board: 9 refused addresses, 2000 reads and 2000 writes each, 36,000 accesses). The
downside is that a wrong address gives no error; to check the path, read `VERSION`
(`0x0001_0000` on every core).

Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); registers:
[docs/REGISTERS.md](docs/REGISTERS.md).

The current default bitstream (with ML-DSA) uses 55,366 LUT (78.5 %), 199 DSP and
85 BRAM after place and route, WNS +1.615 ns at 75 MHz.

## Without a board

```bash
git clone https://github.com/per1x/pqc-crypto-module
cd pqc-crypto-module

python3 -m venv .venv-rtl && ./.venv-rtl/bin/pip install cocotb
brew install icarus-verilog verilator      # or your distribution's packages

./tools/rtl_sim_fast.sh     # day-to-day: per module, Verilator, seconds to minutes
./tools/rtl_sim.sh          # full suite, 272 cocotb tests on Icarus, about 40 min; run before merging
./tools/rtl_lint.sh         # Verilator -Wall + Icarus, every module under hardware/rtl
./tools/rtl_synth_check.sh  # Yosys synthesisability
```

`rtl_lint.sh` currently fails: seven modules (mldsa_axi, mldsa_engine, mldsa_keygen, mldsa_sign, mldsa_verify, mlkem_axi, zu3eg_hsm_top) still have Verilator warnings, mostly constant width extension and unused signals. The other modules are clean.

Host software and the PKCS#11 module (CMake ≥ 3.20, OpenSSL 3). The post-quantum
algorithms come from the bundled mlkem-native / mldsa-native
(`third_party/pqc-native/`, also used by the OP-TEE TA); liboqs is not needed.

```bash
./tools/fetch_vectors.sh && cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

Bitstream (Vivado 2020.1, about 35 min, part `xazu3eg-sfvc784-1-i`):

```bash
vivado -mode batch -source hardware/syn/impl_bitstream.tcl
```

## Usage

### C interface (libsdfe)

`service/` holds the application interface. The example `sdf_demo` contains no
cryptographic implementation (the OpenSSL it links is used only for TLS on the remote
port), so any correct result it prints came from the board.

```c
#include "sdfe.h"

SDFE_HANDLE dev, ses;
uint8_t ek[1600], ct[1600], ss[32];
uint8_t blob[SDFE_SYMKEY_BLOB_MAX], pt[16], out[16];
uint32_t ek_len = sizeof ek, ct_len = sizeof ct, ss_len = sizeof ss, kh, blen = sizeof blob;

SDFE_OpenDevice(&dev);
SDFE_OpenSession(dev, &ses);

SDFE_GenerateRandom(ses, 32, ss);                    /* PL TRNG                                */
SDFE_GenerateKeyPair_MLKEM(ses, SDFE_MLKEM_768,      /* returns the public key and a handle    */
                           ek, &ek_len, &kh);
SDFE_Encapsulate_MLKEM(ses, SDFE_MLKEM_768, ek, ek_len,
                       ss, &ss_len, ct, &ct_len);
SDFE_Decapsulate_MLKEM(ses, kh, ct, ct_len, ss, &ss_len);   /* by handle                   */

SDFE_GenerateKey(ses, /*slot*/ 3, 16, blob, &blen);  /* TA makes an SM4 key, loads it into the PL, returns the blob */
SDFE_Encrypt(ses, SDFE_ALG_SM4, 3, pt, out);         /* one block, computed in the PL          */
SDFE_LoadKey(ses, 3, blob, blen);                    /* a later session loads the same key     */
```

`SDFE_ImportKey` (plaintext import) is still in the interface but always returns
`SDR_NOTSUPPORT`; plaintext keys do not pass through the normal world.

Build and run: `make -C service && ./service/sdf_demo`. Reference:
[docs/API.md](docs/API.md).

Full demo from another machine over the remote port (TCP 9797, mutual TLS):

```bash
./demo/remote/run.sh                           # asks for the board address; demo certificates included
./tools/demo_remote.sh <board-ip> --provision  # switch to your own PKI (once)
```

### Command line: hsmctl

Source [service/hsmctl.c](service/hsmctl.c); the business logic is in
[service/hsmops.c](service/hsmops.c), shared with the HTTP gateway. `hsmctl` reads and
writes files and does PKCS#7 padding; encryption, signing, hashing and random numbers
are done by the module.

- Build: `make -C service hsmctl`. The board binary is cross-compiled on the build machine ([boot/d7/build-apps-d7.sh](boot/d7/build-apps-d7.sh)) and lives at `hsm/hsmctl` on the SD card.
- Where it runs: on the board, talking to `pqchsm_fpgad` over a local UNIX socket. `hsm/HSM_STATUS` must contain `READY=yes`.
- Keystore: the directory in `HSM_KEYSTORE` (default `./keystore`, created if missing), one `<name>.key` file per key, mode 0600. A symmetric key file holds the TA-wrapped blob (60 or 76 bytes); a post-quantum key file holds the public key and the TA-wrapped seed blob. No file contains a plaintext key.

| Command | Does | Runs in |
|---|---|---|
| `hsmctl info` | Core versions and keystore location | — |
| `hsmctl keygen <alg> <name>` | Generate a key into `<name>.key` | TA generates and wraps, loads into the PL |
| `hsmctl list` | Name, algorithm, public key length, blob length, creation time | local |
| `hsmctl show <name>` | Public key digest, SM3 fingerprint, first bytes of the blob | SM3 in the PL |
| `hsmctl encrypt <name> <in> <out>` | CBC + PKCS#7, IV from the PL TRNG; output = 16-byte IV + ciphertext | TA unwraps and loads, PL encrypts block by block |
| `hsmctl decrypt <name> <in> <out>` | Decrypt and check padding | same |
| `hsmctl sign <name> <file> <sig>` | ML-DSA sign, file up to 8 KB | TA unwraps the seed, PL expands the key and signs, then clears it |
| `hsmctl verify <name> <file> <sig>` | ML-DSA verify, public key only | PL |
| `hsmctl encap <name> <ct>` | ML-KEM encapsulate (sender, public key only), prints the shared secret | PL |
| `hsmctl decap <name> <ct>` | ML-KEM decapsulate (receiver), prints the shared secret | TA unwraps the seed, PL computes |
| `hsmctl hash <file>` | SM3, file up to 16 KB | PL |
| `hsmctl random <n>` | 1–256 random bytes | PL |
| `hsmctl backup <bundle>` | Pack the keystore, SM3 per entry | SM3 in the PL |
| `hsmctl restore <bundle>` | Check every entry first; write nothing unless all pass | SM3 in the PL |
| `hsmctl destroy <name>` | Overwrite and delete a key file | local |

`<alg>`: `sm4`, `aes128`, `aes256`, `mldsa44`, `mldsa65`, `mldsa87`, `mlkem512`,
`mlkem768`, `mlkem1024`. Blob sizes: SM4 and AES-128 60 bytes, AES-256 76, ML-DSA 76,
ML-KEM 108.

Exit status: 0 success; 1 error (bad arguments, file I/O, the module refused, e.g. a
modified blob or one from another board); 2 a "no" result (signature does not verify,
or a bundle failed its check and was not restored).

The daemon writes a tagged line to `/media/sd-mmcblk1p2/hsm/hsm-boot.log` after each
layer returns successfully: `[TEE]` TA work, `[TEE→PL]` key material written into the
PL by the TA, `[PL]` computation in the PL with timing, `[NW]` daemon-only work (passing
blobs), `[HTTP]` requests received by the gateway.

Limits: a backup restores only on the same board (cross-device migration is not
implemented). Each symmetric command loads the key into the PL once through the TEE
(about 1.3 ms measured). Every register access goes through EL3, so throughput is low:
AES-256-CBC measures about 0.9 MB/s.

### HTTP: hsm_httpd

Source [service/hsm_httpd.c](service/hsm_httpd.c), built on the same `hsmops.c`.
Started at boot by `hsm-boot.sh`, keystore `/media/sd-mmcblk1p2/keys`.

| Port | Protocol | Who can connect |
|---|---|---|
| `127.0.0.1:8080` | HTTP | Loopback only, so only processes on the board; no authentication, same trust as running `hsmctl` there |
| `:8443` | HTTPS with client certificates | Client certificate must be issued by `hsm/pki/hsm_ca.crt`; not opened if the certificate files are missing |

Usage: `hsm_httpd [https_port [local_http_port]]`, defaults 8443 and 8080, 0 disables
that port.

| Method | Path | Body | Response |
|---|---|---|---|
| GET | `/v1/info` | — | device info |
| GET | `/v1/keys` | — | key list |
| POST | `/v1/keys` | `{"name","alg"}` (`alg` as for `hsmctl keygen`) | 201, key info with public key |
| GET / DELETE | `/v1/keys/{name}` | — | show / delete a key |
| POST | `/v1/keys/{name}/encrypt` | `{"plaintext"}` or `{"plaintext_b64"}` | `ciphertext_b64` (IV + CBC ciphertext) |
| POST | `/v1/keys/{name}/decrypt` | `{"ciphertext_b64"}` | `plaintext_b64`, plus `plaintext` if it is UTF-8 |
| POST | `/v1/keys/{name}/sign` | `{"message"}` or `{"message_b64"}` | `signature_b64` |
| POST | `/v1/keys/{name}/verify` | same, plus `"signature_b64"` | `{"valid": true/false}` |
| POST | `/v1/keys/{name}/encapsulate` | `{}` | `ciphertext_b64`, `shared_secret_hex` |
| POST | `/v1/keys/{name}/decapsulate` | `{"ciphertext_b64"}` | `shared_secret_hex` |
| POST | `/v1/sm3` | `{"data"}` or `{"data_b64"}` | `sm3_hex` |
| POST | `/v1/random` | `{"length"}`, at most 1024 | `random_hex` |

Status codes: unknown key 404, duplicate name 409, bad request 400, TA refused to load
the key 422. Add `?trace=1` to get the request's layer log in a `trace` array.

The board has no curl and busybox `wget` cannot POST; use `nc`:

```bash
printf 'POST /v1/sm3 HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 14\r\nConnection: close\r\n\r\n{"data":"abc"}' | nc 127.0.0.1 8080
```

### On-board demo

`sh /media/sd-mmcblk1p2/demo/run.sh` runs the business flow (device info, key
management, encryption, signatures, key encapsulation, hash and random, backup and
restore, security checks), then repeats the main operations through the HTTP API.
Each command is followed by the layer log it produced. Convert the output to a web
page with `python3 tools/demo_to_html.py <output.txt> <out.html>`; a captured run is
[docs/demo/business_demo.html](docs/demo/business_demo.html).

## Measured results

Measured on the board in the configuration described in
[docs/SECURITY.md](docs/SECURITY.md).

| Check | Result |
|---|---|
| ML-KEM 512 / 768 / 1024 vs NIST ACVP | 20 / 20 byte-exact |
| ML-DSA 44 / 65 / 87 vs ACVP (including parameter-set switches in one bitstream) | 32 / 32 |
| On-board self-test (symmetric, SM, boundary, AxPROT, TRNG) | 24 / 24 |
| No read path from the symmetric key registers: 256-byte address space scanned on two instances | key words found 0 times; ciphertext correct |
| AxPROT gate (on the development bitstream, to have a `SECURE_ONLY=0` core for comparison) | EL3 reads a `SECURE_ONLY=1` core as `0x0001_0000`; EL1 non-secure is refused at the same address; the same EL1 reads a `SECURE_ONLY=0` core |
| Normal world writes the symmetric-key load port directly (development bitstream, as booted) | all 11 writes dropped, `LOAD_REJ` 0 → 11, entry stays empty |
| Daemon wire-protocol fuzzing | 41 / 41 |
| TRNG min-entropy, 1,048,576 pre-conditioning samples | 0.871234 bit/sample; RCT cutoff 47, APT cutoff 672 |
| Decaps timing, valid vs implicit-reject, 200 runs each | median difference 0.000 % |
| ML-KEM-512 throughput at 75 MHz | 924 / 1339 / 1018 ops/s (KeyGen / Encaps / Decaps) |
| cocotb regression | 272 tests |

Methods: [docs/TESTING.md](docs/TESTING.md). Raw board output: [board/logs/](board/logs/).

## Limitations

- **The ACVP KeyGen path still outputs the private key.** On the business path the
  private key does not leave the PL: the TA generates and wraps the seed and writes it
  into the PL seed port through the secure world, and each operation (`MODE.CHAIN`)
  expands it and has the hardware clear it. The KeyGen that takes `d‖z` from software
  is kept because ACVP has to reproduce and check the private key from a given seed;
  its `dk` can be read from `OUT_DATA`. `CTRL.SEED_LOCK` restricts KeyGen to the seed
  port, but it is cleared by a power cycle and the daemon does not set it.
- **root can still make the hardware compute.** The daemon builds operation sequences
  in the normal world and sends them through the EL3 whitelist. Key loading is
  secure-world only, so the normal world can neither read nor inject keys, but it can
  use keys that are already loaded, and any process that can open a TA session as
  `LOGIN_USER` can have the TA load a key from its own blob. Per-caller authorisation
  is not implemented.
- **The KEK root can be derived.** The TA's HUK is SHA-256(Device DNA); no eFUSE is
  burned. Device DNA is readable (JTAG, for example), so someone with the DNA and a blob
  can unwrap it. A real HUK depends on authenticated boot (see the encrypted-boot item).
  On the host side, `src/crypto/kdr.c` is a fixed stub constant in DEV builds (left out
  of PRODUCTION builds, which fail when no root key is provided); `PQCHSM_KDR=device-dna`
  switches to a root bound to Device DNA, which prevents cloning but gives no secrecy.
- **No SM2.** SM4 and SM3 are implemented; SM2 is not.
- **Performance is not a goal.** Throughput figures include per-byte software AXI
  access and are given to compare parameter sets (about 1 : 1.5 : 2.1), not as the
  hardware's limit.
- **No protection against power or EM side channels.** Constant-time work covers timing
  only. Masking cannot be validated without a side-channel bench, so none was added.
- **No encrypted boot.** BBRAM is blocked by the board (no battery on `VCC_BATT`); the
  PUF black key is rejected by the BootROM before the PUF is used. RSA authenticated boot
  works without eFUSE, but it is not a root of trust: `bh_auth_enable` means skipping
  the PPK hash check against eFUSE. Preventing image replacement requires burning eFUSE,
  and this project does no irreversible programming. See [docs/SECURITY.md](docs/SECURITY.md).
- **PS-side XMPU / XPPU do not cover the PL window.** Per UG1085 no PS protection unit
  covers `0x8000_0000`, so the PL firewall is the only access control on this path, and
  the address decode is one-to-one with no mirrors.
- **The DDR-side XMPU only prevents reads.** The XMPU does not block transactions; it
  marks them poisoned and the endpoint refuses them (XAPP1320 v3.0, p. 10). BL31
  configures it at boot, so a normal-world read of OP-TEE secure memory gets a bus
  error. It does not stop root from using the SiP to make the hardware compute; the SiP
  does not check the caller.

## Repository layout

```
hardware/rtl/       Verilog-2001 crypto cores: mlkem/ mldsa/ keccak/ sym/ trng/ bus/ board/
hardware/platform/  Non-crypto PL logic (fan control); same bitstream, no shared signals
hardware/tb/        cocotb testbenches, simulation tops, lint-only vendor primitive stubs
hardware/model/     Python reference model, independent checks, vector export
hardware/syn/       Vivado synthesis and bitstream scripts
service/            daemon, libsdfe, hsmctl, hsm_httpd, sdf_demo
tee/                OP-TEE TA and its normal-world client: key generation, wrapping, seed and symmetric-key loading
boot/               BL31 patches (SiP), OP-TEE patches, build scripts for the current boot image (boot/d7/)
board/              On-board programs, boot scripts, kernel modules, demo scripts, raw logs
demo/               Demos run from another machine (remote SDF, HTTP) and the management-function demo
include/ src/ cli/  Host software: keystore, backup, audit, PKCS#11 front end
tests/              Host unit, integration, KAT and fuzz tests
tools/              Regression scripts, SP 800-90B estimators, static checks
docs/               Architecture, API, security model, testing, register maps
```

## Documentation

| Document | Contents |
|---|---|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layering, address map, clocks, key hierarchy, hardware/software interface |
| [API.md](docs/API.md) | SDF-style interface and PKCS#11 v3.2 front end |
| [SECURITY.md](docs/SECURITY.md) | Trust boundary, threat model, what has and has not been shown |
| [REGISTERS.md](docs/REGISTERS.md) | Register definitions for every AXI slave |
| [TESTING.md](docs/TESTING.md) | What is tested, how, and how to reproduce it |
| [USAGE.md](docs/USAGE.md) | Building, running, deploying, operating the board |
| [reference/](docs/reference/) | Phase reports, security policy draft, offline deployment, constant-time audit |

Chinese versions sit next to each file as `*.zh-CN.md`. `./tools/pdf/build-pdf.sh`
combines architecture, API, registers, security and testing into one PDF.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Before touching the board, read
[the irreversible-operation red lines](docs/SECURITY.md#-irreversible-operation-red-lines).
To report a vulnerability see [SECURITY.md](SECURITY.md); the problems listed under
Limitations are already known.

## License

[Apache-2.0](LICENSE), chosen over MIT for its explicit patent grant and patent
retaliation clause.

`third_party/pkcs11-v3.2/` contains three OASIS headers, included unmodified under
their own terms.
