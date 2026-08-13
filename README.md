**English** · [中文](README.zh-CN.md)

# pqc-crypto-module

A post-quantum cryptographic module prototype: key storage, slot management, backup
and recovery, tamper-evident audit logging, and a PKCS#11 v3.2 front end — plus a
complete hardware crypto engine in programmable logic, **built and validated on real
silicon** (Xilinx XCZU3EG).

> **Status: research prototype.** The cryptographic boundary is **hardware** — an
> AxPROT-gated AXI firewall in programmable logic — and it has been **proven in both
> directions on the board**: the secure world (EL3) reads a `SECURE_ONLY=1` core, the
> normal world is refused at the bus, and that same normal world reads a
> `SECURE_ONLY=0` core successfully.
>
> The host software (`src/`, `cli/`, `demo/`) is the control plane **outside** that
> boundary: slots, sessions, the keystore file, backup and recovery, and the PKCS#11
> front end. What does and does not cross the boundary is stated precisely under
> [Security model and limitations](#security-model-and-limitations) — including the
> parts that still do.
>
> This is not a certified module. Do not use it to protect anything real. Full method
> and raw logs: [密码机原型-说明文档.md](docs/密码机原型-说明文档.md)
> ([PDF](docs/密码机原型-说明文档.pdf)).

## Overview

Post-quantum algorithms are standardised (FIPS 203 / 204), but the surrounding
machinery — how keys are stored, wrapped, backed up, revoked, and exposed to
applications — is where a cryptographic module actually lives or dies. This project
builds that machinery around ML-KEM and ML-DSA, in the shape a real device would take.
The algorithm cores have since been lifted onto a Zynq UltraScale+ SoC: ML-KEM
512/768/1024, AES-128/256, SM4, SM3, a ring-oscillator TRNG, a key vault and the access
boundary now run in programmable logic on real hardware.

Two consequences of that goal explain most of the design:

- **Everything is behind a handle.** No API in `include/pqchsm/` returns private key
  material. The keystore, the slot manager, and the PKCS#11 front end all operate on
  opaque handles, so tightening the boundary later does not change any caller.
- **The hardware seam exists from day one.** Cryptographic operations go through a
  vtable (`pqc_backend_t`) and, below it, an AXI-style register interface
  (`accel_transport_t`). Four transports implement that interface identically: a
  software stub, a Verilator-simulated RTL backend, the same RTL driven over real
  AXI4-Lite and AXI4-Stream transactions, and `/dev/mem` + `mmap` on the board.
  Swapping between them changes nothing above the seam.

Every cryptographic operation on the live path uses
[liboqs](https://github.com/open-quantum-safe/liboqs) or OpenSSL; no primitive is
hand-rolled for production use. The hand-written NTT and Keccak implementations that do
exist (`src/hal/accel_stub.c`, `hardware/model/`) serve only as reference points for
RTL comparison and are never used to protect key material. Correctness is pinned to
NIST ACVP vectors.

## Architecture

```
                PKCS#11 v3.2 shared library  (src/p11)
                Daemon / CLI / admin tools   (cli)
 ──────────────────────────────────────────────────────────────────
  slot manager    keystore      backup / recovery    audit chain
  (src/slot)      (src/store)   (src/backup)         (src/audit)
    FSM,          AES-256-GCM   Shamir M-of-N        SHA3-256 chain
    sessions,     wrapping,     over GF(256),        + ML-DSA anchor
    handles,      atomic        device-bound KEK       signing
    ACLs          file I/O      vs portable BEK
 ──────────────────────────────────────────────────────────────────
                  pqc_backend_t vtable  (include/pqchsm/pqc.h)
                             │
             ┌───────────────┴────────────────┐
       liboqs backend                  register-interface backend
       (src/crypto)                    (src/hal/pqc_accel.c)
                                               │
                             accel_transport_t (include/pqchsm/accel.h)
                                               │
            ┌──────────────┬───────────┴───────────┬──────────────┐
      software stub   Verilator RTL          AXI over RTL        mmap
     (accel_stub.c) (accel_verilator.c)     (accel_axi.c)   (accel_mmap.c)
                            │                     │
                   hardware/rtl cores    hardware/rtl/bus/pqc_accel_axi
                                          (AXI4-Lite + AXI4-Stream)
```

### Key hierarchy

A device-bound root (`KDR`, currently a stub — see limitations) derives a **KEK** that
wraps keys at rest. Backup instead uses a **BEK** derived from a randomly generated
recovery master key, split with Shamir's scheme over GF(256) using a constant-time
multiply. The distinction is deliberate: KEK-wrapped material cannot leave the device,
BEK-wrapped material is portable to a replacement device, and a slot must opt in to
being backed up at all.

### Audit chain

Every state transition appends a record whose hash covers the previous record's hash.
A pure hash chain only guarantees that tampering propagates forward, so the chain head
is additionally signed with an ML-DSA device identity key and anchored outside the
device — otherwise rewriting the whole file is undetectable.

### Hardware seam

`accel.h` fixes the register map (`CTRL`, `STATUS`, `MODE`, `PARAM`, `IN_LEN`,
`OUT_LEN`, `ERRCODE`) before any RTL was written, so interface bugs surface in software
first. That contract is now implemented in hardware as well: `pqc_accel_axi` exposes it
over AXI4-Lite for control and AXI4-Stream for bulk data, and
[docs/register-map.md](docs/register-map.md) states the semantics both sides are
written against — START self-clearing, DONE latched as a level, status registers
written by hardware and read-only to software.

The RTL covers the arithmetic of both algorithms plus the bus interface. It is plain
inferrable Verilog-2001 throughout: no vendor primitive is instantiated anywhere, so
the same sources target Xilinx, Intel, or Lattice unchanged.

| Group | Cores |
|---|---|
| ML-KEM | `ntt_core` (7-layer, 1153 cycles), `mlkem_basemul`, `mlkem_compress`/`decompress`, `mlkem_cbd2`/`cbd3`, `mlkem_rej_pair`/`rej_uniform`, `mlkem_encode12`/`decode12` |
| ML-DSA | `mldsa_ntt_core` (8-layer, 1025 forward / 1281 inverse), `mldsa_mont_reduce`, `mldsa_reduce32`, `mldsa_caddq`, `mldsa_power2round`, `mldsa_decompose`, `mldsa_make_hint`/`use_hint`, `mldsa_rej_uniform`/`rej_eta` |
| Keccak | `keccak_f1600` (single-round iterative, 24 cycles) |
| Bus | `axi4lite_regs`, `pqc_accel_axi` |
| Noise source | `trng_health` (SP 800-90B repetition-count and adaptive-proportion tests) |

SHA3 and SHAKE are built as a sponge in C on top of the permutation, so the entire
SHA3/SHAKE path can be run against simulated RTL and compared byte-for-byte with
OpenSSL. Complete ML-KEM and ML-DSA operations have no hardware implementation; the
accelerator reports "unsupported" for those modes rather than silently substituting
software.

## Features

- **ML-KEM-512/768/1024 and ML-DSA-44/65/87**, verified against 390 NIST ACVP vectors
  byte-for-byte (vectors pinned to a specific ACVP-Server commit).
- **Slot/token/object/session model** with an explicit FSM transition table, role-based
  access control, PIN lockout, and generation counters that invalidate stale handles.
- **Encrypted keystore** — AES-256-GCM wrapping with metadata as AAD, whole-file KMAC,
  and atomic `tmp → fsync → rename → fsync(dir)` writes.
- **M-of-N backup and recovery** across devices, with per-slot policy controlling
  whether a key may be backed up at all.
- **Tamper-evident audit log** with ML-DSA anchor signing.
- **Secure key injection** — a one-time session key is encapsulated to the device's own
  ML-KEM public key, so plaintext key material never appears on the wire.
- **PKCS#11 v3.2 front end** exposing native `CKM_ML_KEM` / `CKM_ML_DSA` mechanisms,
  including `C_EncapsulateKey` / `C_DecapsulateKey` reachable via `C_GetInterface`.
- **RTL cores with independent verification** — cocotb testbenches check against the
  published Keccak all-zero permutation vector and against `hashlib`/OpenSSL, not only
  against this project's own reference model.

## Repository layout

```
├── include/pqchsm/     Public headers — the API surface. No private key crosses it.
├── src/
│   ├── crypto/         liboqs binding, KDF, key derivation root (KDR)
│   ├── slot/           Slot FSM, sessions, metadata, persistence
│   ├── store/          AES-256-GCM wrapping, keystore file format
│   ├── backup/         Shamir splitting, backup/restore, key injection
│   ├── audit/          Hash chain and ML-DSA anchoring
│   ├── hal/            Accelerator abstraction: stub, Verilator, register semantics
│   ├── p11/            PKCS#11 v3.2 shared library
│   ├── proto/          TLV command protocol
│   └── util/           Secure zeroing, locked allocation, KAT parsing
├── cli/                Daemon, client CLI, admin tool
├── tests/              Unit, integration, KAT, and fuzz targets
├── demo/               PKCS#11 provider demos (Python, Java)
├── hardware/
│   ├── rtl/            Verilog sources: mlkem/, mldsa/, keccak/, sym/, bus/, trng/, board/
│   ├── tb/cocotb/      cocotb testbenches (197 tests) and simulation-only top levels
│   ├── tb/lint/        Vendor-primitive stubs — lint only, never synthesised
│   ├── model/          Python reference model, vector export, independent oracles
│   └── syn/            Vivado scripts: out-of-context synthesis and the full
│                       RTL-to-bitstream implementation flow (with boot-time assertions)
├── fpga/fan_ctrl/      PL fan temperature control — deliberately separate from the
│                       crypto RTL; same bitstream, no shared signals
├── board/              On-board test programs, payloads, and the PL harness
│                       (every command that touches the PL goes through it)
├── boot/atf/           ATF/BL31 patches: the EL3 SiP used to close the access-gate proof
├── tools/              Vector fetching, benchmarks, profiling, regression scripts
├── third_party/        Vendored OASIS PKCS#11 v3.2 headers (unmodified)
└── docs/               Architecture, PKCS#11, register map, algorithms, security policy, testing
```

## Documentation

| Document | Contents |
|---|---|
| [architecture.md](docs/architecture.md) · [中文](docs/architecture.zh-CN.md) | Layering, key hierarchy, keystore format, audit chain, hardware abstraction, key injection |
| [pkcs11.md](docs/pkcs11.md) · [中文](docs/pkcs11.zh-CN.md) | Mechanisms, object model, vendor attributes, key import, KEM operations, configuration |
| [constant-time.md](docs/constant-time.md) · [中文](docs/constant-time.zh-CN.md) | Constant-time audit scope and method, findings, zeroization checks, what is not claimed, how to reproduce |
| [register-map.md](docs/register-map.md) · [中文](docs/register-map.zh-CN.md) | The accelerator register contract: address map, behavioural clauses, data plane, operation codes |
| [algorithms.md](docs/algorithms.md) · [中文](docs/algorithms.zh-CN.md) | Algorithm inventory, parameter sets, key and SSP inventory, validation evidence |
| [security-policy.md](docs/security-policy.md) · [中文](docs/security-policy.zh-CN.md) | FIPS 140-3 / GM/T 0028 security policy draft, with an explicit gap list |
| [testing.md](docs/testing.md) · [中文](docs/testing.zh-CN.md) | What is tested, by what means, and how to reproduce every number quoted here |
| [deployment.md](docs/deployment.md) · [中文](docs/deployment.zh-CN.md) | Deployment on an intranet Linux host, including obtaining every dependency offline |
| [zynq-port.zh-CN.md](docs/zynq-port.zh-CN.md) (中文) | Porting to a Zynq UltraScale+ MPSoC: staging and dependencies, mapping the software boundary onto silicon, irreversible steps |
| **[密码机原型-说明文档.md](docs/密码机原型-说明文档.md)** (中文, [PDF](docs/密码机原型-说明文档.pdf)) | **The FPGA line's delivery document**: architecture, code guide, every test result with the raw logs embedded, and a per-item blocked list |
| [fpga-进展.md](docs/fpga-进展.md) (中文) | Stage-by-stage engineering log for the FPGA line (S1–S7, P6, P7): what was built, what broke, and why each assertion exists |
| [hardware/README.md](hardware/README.md) | RTL modules, verification strategy, simulator choice |
| [demo/README.md](demo/README.md) | Provider demos and client-library compatibility |

## Building

Requirements: CMake ≥ 3.20, a C11 compiler, OpenSSL 3, and liboqs.

```bash
brew install liboqs openssl@3 cmake        # macOS
# Debian/Ubuntu: liboqs from source; libssl-dev, cmake, ninja-build for the rest
```

```bash
./tools/fetch_vectors.sh                   # download and flatten NIST ACVP vectors
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Optional components are detected, not required. Without Verilator the simulated RTL
backend is simply not compiled in and `accel_transport_verilator()` returns `NULL`;
tests that need `cocotb`, `iverilog`, or `pkcs11-tool` skip themselves with a message
rather than failing.

### Additional checks

```bash
python3 tools/ct_audit.py      # constant-time source audit (--self-test for the controls)
python3 tools/check_zeroize.py # zeroization structure check (--self-test for the controls)
./tools/rtl_sim.sh          # cocotb regression (Icarus Verilog)
./tools/aarch64_test.sh     # full rebuild and regression in an aarch64 Linux container
./tools/fuzz.sh             # libFuzzer targets (requires LLVM clang)
./tools/profile.sh          # sampling profile
./build/pqchsm-bench        # algorithm-level baseline
./build/pqchsm-prim-bench   # per-primitive cost and measured RTL cycle counts
```

## Running the demos

Both demos drive the shared library through the full lifecycle: initialise the token,
set PINs, log in, generate ML-DSA and ML-KEM keys into slots, sign and verify, read
attributes, and enumerate objects.

```bash
cmake --build build --target pqchsm-p11

# Python, via PyKCS11
python3 -m venv .venv-p11 && ./.venv-p11/bin/pip install -q PyKCS11
./.venv-p11/bin/python demo/python/pqchsm_demo.py

# Java, via the JDK 22+ FFM API (no external dependency)
java --enable-native-access=ALL-UNNAMED demo/java/PqcHsmDemo.java \
     "$PWD/build/pqchsm-pkcs11.dylib"
```

Both use the low-level PKCS#11 binding directly. Higher-level provider frameworks are
**not** used, because they do not yet support these mechanisms — see
[demo/README.md](demo/README.md) for details and for a probe that demonstrates it.

## Security model and limitations

This is a prototype. The following are accurate statements about what it does and does
not do.

**The cryptographic boundary is hardware.** The ML-KEM cores, the symmetric cores
(AES-128/256, SM4, SM3), the ring-oscillator TRNG, the key vault and the AXI firewall
all live in the FPGA's programmable logic. The firewall gates on `AxPROT[1]`, and the
gate has been proven in **both** directions on the board: the secure world (EL3,
`AxPROT[1]=0`) reads a `SECURE_ONLY=1` core and gets `0x00010000`; the normal world
(EL1-NS, `AxPROT[1]=1`) is refused at the bus (SIGBUS/DECERR) — while that *same*
normal world reads a `SECURE_ONLY=0` core successfully, so the difference is the gate
and not an unreachable address.

**What does and does not cross that boundary — stated precisely, because it is easy to
over-claim.**

- **Symmetric keys loaded into the key vault do not cross it.** Measured by scanning
  256 bytes of each of two slaves (48 readable, 80 refused by the firewall): not one of
  the key's four words appeared anywhere, while the ciphertext those keys produced was
  correct. Both halves are needed — either alone proves nothing.
- **ML-KEM private keys currently do cross it.** `KeyGen` returns `ek ‖ dk` over AXI,
  because that is what checking against NIST ACVP vectors requires. A production form
  would keep `dk` inside the boundary or export it only wrapped. This is an interface
  decision, not a firewall failure, and it is not fixed here.
- **Host-side key handling is outside the boundary.** The keystore file, its AES-GCM
  wrapping, slot metadata and the PKCS#11 layer run in host software. For those,
  plaintext key material exists in process memory, buffers are zeroed and `mlock`-ed
  where possible, and nothing defends against an attacker who can read that address
  space. [constant-time.md](docs/constant-time.md) records what the constant-time and
  zeroization audits cover and — more usefully — what they do not.

**The host-side key derivation root is a stub.** `src/crypto/kdr.c` contains a fixed
32-byte constant whose literal text reads `PQC-HSM STUB KDR -- NOT SECRET!!`. A real
device takes this from eFUSE, BBRAM, or a PUF. On this board neither is available:
eFUSE is irreversible and there is only one board, BBRAM needs JTAG. Device binding is
therefore not real, and that is a deliberate, recorded limit rather than an oversight.

**The constant-time work covers timing only.** Checked on the board: the valid and
implicit-reject Decaps paths differ by 0.000 % at the median over 200 runs each. Power
and electromagnetic side channels are **not** addressed and are not claimed.

**Other known gaps.** The audit module assumes a single writer and does not lock the
file. Shamir share checksums are unkeyed: they detect corruption, not tampering. SO PIN
failures increment a counter but do not lock the slot — locking it would brick the
device, and the fallback is M-of-N recovery. `CKM_HASH_ML_DSA_*` is not implemented, so
PKCS#11 multi-part signing buffers the whole message rather than streaming a digest.

## Testing

| Check | Result |
|---|---|
| `ctest` | 45 / 45 |
| Assertions | 4059 |
| NIST ACVP vectors | 390 byte-exact, 60 explicitly skipped |
| ASan + UBSan | 45 / 45 |
| ThreadSanitizer | 0 races (validated by removing locks: 9 reported) |
| macOS `leaks` | 0 leaks |
| libFuzzer | 1.38 M executions, no crashes |
| aarch64 Linux (GCC 12) | 45 / 45 |
| cocotb RTL regression | 156 tests across 26 top levels |
| RTL lint (Verilator `-Wall` + Icarus) | 31 modules, 0 warnings |
| RTL synthesisability (Yosys) | 31 modules, all synthesise |

Two habits run throughout the test sources:

- **Independent oracles.** A result is not trusted because it matches this project's own
  model. KMAC is checked against the NIST document, OpenSSL, and a separate Keccak. The
  NTT is checked against a schoolbook negacyclic convolution that never touches the
  twiddle table, and by reconstructing ML-KEM key generation and reproducing ACVP
  `ek`/`dk` byte-for-byte. Keccak is checked against the published all-zero permutation
  vector and against `hashlib`/OpenSSL.
- **Structural checks over habits.** Properties that no functional test can see —
  the key derivation root having no read-back interface, no secret-dependent branch or
  index in `src/`, every key-material field wiped by its destructor — are expressed as
  scanners wired into `ctest`, each of which self-tests on synthetic samples before it
  is allowed to report a clean scan. See [constant-time.md](docs/constant-time.md).
- **Negative controls.** Assertions are validated by breaking something and confirming
  the test fails — perturbing a twiddle factor, dropping an NTT layer, flipping a bit in
  a Keccak round constant, removing a lock under TSan, adding a fake key-readback
  function, timing a deliberately early-returning comparison, probing a stack frame
  that was never wiped.

## What is built, and what is not

Every claim below is attached to its evidence, so it can be checked rather than
believed. Method and raw logs:
[密码机原型-说明文档.md](docs/密码机原型-说明文档.md)
([PDF](docs/密码机原型-说明文档.pdf)).

| Capability | Status and evidence |
|---|---|
| A complete ML-KEM dataflow in RTL | **Done.** ML-KEM 512/768/1024 KeyGen/Encaps/Decaps, byte-exact against NIST ACVP vectors **on silicon** (20/20). Parameter set selected by a register field; lengths are derived in RTL, so software cannot report a wrong one |
| Synthesis and timing closure on the target device | **Done.** Full RTL-to-bitstream flow. 35592 LUT (50.44 %), 25916 FF, 15.5 BRAM, 140 DSP, **WNS +3.174 ns / WHS +0.001 ns** @ 75 MHz |
| Security boundary in programmable logic | **Done.** AxPROT-gated AXI firewall, key vault whose keys leave only over a private wire. Proven both directions on the board: EL3 reads a `SECURE_ONLY=1` core, EL1-NS is refused (SIGBUS/DECERR), while the *same* normal world reads a `SECURE_ONLY=0` core — so the difference is the gate, not reachability |
| Key derivation root in eFUSE / BBRAM / PUF | **Not done, and not planned on this board.** eFUSE is irreversible and there is only one board; BBRAM needs JTAG. See the blocked list in the delivery document |
| A ring-oscillator noise source with an SP 800-90B assessment | **Done.** 1,048,576 **pre-conditioning** samples exported from the board; SP 800-90B non-IID estimators give **H = 0.871234 bits/sample**. The measured value sets the health-test cutoffs (RCT 47, APT 672); cutoffs assumed from H = 0.5 would have left the APT test unable to fire |
| End-to-end throughput | **Measured.** ML-KEM-512 924 / 1339 / 1018 ops/s (KeyGen / Encaps / Decaps); 768 and 1024 scale ≈ 1 : 1.5 : 2.1, matching the k = 2/3/4 workload |

Still open, with the reason attached:

1. **ML-DSA whole cores.** The operators exist (13 modules, verified against the
   reference model) but are not chained into KeyGen/Sign/Verify.
2. **Boot-time persistence of the PL configuration** — needs JTAG, because the only
   remaining clean route writes the golden `BOOT.BIN`.
3. ~~**XMPU/XPPU configuration.**~~ **Closed: structurally not applicable.** UG1085
   v2.5 settles it — XPPU's aperture table (Table 16-10) enumerates every aperture and
   `0x8000_0000` is in none of them, and FPD_XMPU is not on the `M_AXI_HPM0_LPD` path
   (p1092: the PL is reached "without the FPD"). No PS-side protection unit covers the
   PL window at all. That reinforces the architecture rather than exposing a gap:
   **the AxPROT-gated firewall in the PL is the only enforcement point on this
   routing**, which is exactly why the address decode must be one-to-one with no
   mirrors. See §5.5 of the delivery document.
   (Incidentally measured: PS-side protection on this board is entirely unconfigured —
   XPPU `CTRL=0`, all 400 permission entries at their reset default. Unrelated to the
   above: even configured, it would not reach the PL.)
4. **Power/EM side channels.** Not attempted, and not claimed. The constant-time work
   covers timing only, and was checked on the board (median difference 0.000 % between
   the valid and implicit-reject Decaps paths).

## License

[Apache-2.0](LICENSE). Chosen over MIT for its explicit patent grant and patent
retaliation clause ; patents are a real exposure in this field and MIT is silent on
them.

The three headers under `third_party/pkcs11-v3.2/` are OASIS documents, included
unmodified under their own terms.
