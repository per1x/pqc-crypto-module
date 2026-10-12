**English** · [中文](README.zh-CN.md)

# Hardware abstraction layer

This directory sits between the host-side cryptographic code (slot manager,
keystore, backup, audit, PKCS#11) and whatever performs the arithmetic. The code
above it works with handles and does not depend on which backend is installed.

## Two interfaces

`pqc_backend_t` (`include/pqchsm/pqc.h`) is the vtable for
keygen/encaps/decaps/sign/verify. There are three implementations:

| Implementation | Where | What it does |
|---|---|---|
| `src/crypto/pqc_native.c` | software | Vendored mlkem-native / mldsa-native, the same sources the OP-TEE TA uses |
| `pqc_accel.c` | this directory | Drives the `accel_transport_t` register interface below |
| `pqc_sdfe.c` | this directory | Calls the board's cores through `libsdfe` and `pqchsm_fpgad` |

`accel_transport_t` (`include/pqchsm/accel.h`) is an AXI-style register map:
`CTRL`, `STATUS`, `MODE`, `PARAM`, `IN_LEN`, `OUT_LEN`, `ERRCODE`, with bulk data
moved separately. It targets the `pqc_accel_axi` accelerator in
`hardware/rtl/bus/`, which is not part of the board bitstream
(`hardware/rtl/board/zu3eg_hsm_top.v` explains why). On the board the ML-KEM and
ML-DSA cores are reached through `pqc_sdfe.c`.

## Files

| File | Role |
|---|---|
| `pqc_accel.c` | `pqc_backend_t` over the register interface; also `accel_ntt()`, `accel_keccak_f1600()`, `accel_shake()` |
| `accel_stub.c` | Software transport. Implements the register semantics and calls `pqc_backend_native()` internally. Always available |
| `accel_verilator.c` | Transport that drives Verilator-simulated RTL. Compiled only when Verilator is present, otherwise `accel_transport_verilator()` returns `NULL` |
| `accel_axi.c` | Transport that drives the simulated RTL with AXI4-Lite register transactions and AXI4-Stream packets (the contract in `docs/REGISTERS.md`). Same Verilator condition; otherwise `accel_transport_axi()` returns `NULL` |
| `verilator/ntt_sim.cpp`, `keccak_sim.cpp`, `axi_sim.cpp` | C wrappers around the Verilated `ntt_core`, `keccak_f1600` and the AXI-fronted accelerator |
| `accel_mmap.c` | `/dev/mem` + `mmap` transport. Returns `NULL` unless `PQCHSM_ACCEL_MMAP_BASE` and `PQCHSM_ACCEL_MMAP_BUF` are defined at build time. Has not been run against real PL |
| `hwrng.c` | Driver for the TRNG register contract (`trng_axi`, `docs/REGISTERS.md`) |
| `hwrng_stub.c`, `hwrng_stub.h` | Software model of the `trng_axi` registers. The FIFO is filled by OpenSSL, so it models the interface and provides no hardware entropy. The header has fault-injection hooks for tests |
| `hwrng_mmap.c` | `/dev/mem` + `mmap` transport for the TRNG. Returns `NULL` unless `PQCHSM_HWRNG_MMAP_BASE` is defined at build time. In the default bitstream (`SECURE_ONLY=1`) the normal world cannot reach these registers, so it is only useful with the debug bitstream |
| `hwrng_sdfe.c` | Byte-level entropy source `hwrng_byte_source_sdfe()`: fetches TRNG bytes through `libsdfe` and `pqchsm_fpgad`. This is the path that works on the board |
| `pqc_sdfe.c` | `pqc_backend_sdfe()`: ML-KEM and ML-DSA on the PL through the service. Encaps and the `*_hw` calls (keypair, decaps, sign by handle) go to the board. The byte-form keypair, decaps and sign return `PQC_ERR_UNSUPPORTED` for ML-KEM/ML-DSA so that private keys are never handed out as bytes. Verify stays in software, which gives an independent check of the hardware signatures |
| `sdfe_conn.c`, `sdfe_conn.h` | The one connection to the service that `hwrng_sdfe.c` and `pqc_sdfe.c` share (local socket, or mTLS TCP when `PQCHSM_SDFE_HOST` and `PQCHSM_SDFE_PKI` are set; `PQCHSM_SDFE_PORT` and `PQCHSM_SDFE_DEVICE_CN` are also read). The daemon serves one connection at a time, so two connections from one process stall |

The PKCS#11 module selects the board paths with environment variables
(`src/p11/p11_module.c`): `PQCHSM_HWRNG=sdfe` installs the hardware entropy
source, `PQCHSM_BACKEND=sdfe` installs `pqc_backend_sdfe()`.

## Entropy source

`hwrng.h` is a second transport interface and is kept apart from the `accel.h`
opcodes. The accelerator is command/complete while the TRNG free-runs, the TRNG
usually has stricter access control, and a TRNG alarm has to be reported
independently of the accelerator's busy state. The RTL keeps them in separate
slaves for the same reasons.

Once a hardware entropy source (register transport or byte source) is
installed, `pqc_random_bytes()` draws from it and returns an error when it
fails; it does not fall back to software. The software stub reports
`is_hardware = 0`, so installing it leaves `pqc_random_bytes()` on OpenSSL.
With a byte source installed, the register-level calls (`hwrng_status`,
`hwrng_zeroize`, health counters) are unavailable.

## SHAKE and Keccak

`accel_shake()` has two paths:

- Mode 10 (`ACCEL_MODE_SHAKE`): the whole sponge runs in the PL and only the
  message and digest cross the bus. It is used when the message and the output
  both fit in `ACCEL_SHAKE_MAX` (512 bytes, the PL buffer size). Rate,
  domain-separation suffix and output length are packed into `PARAM`
  (`ACCEL_SHAKE_PARAM`).
- Mode 9 (`ACCEL_MODE_KECCAK_F1600`): used when the message or output is larger,
  or when the transport answers `ERRCODE=3` (mode not implemented). Padding and
  absorb/squeeze framing are done in C and only the permutation runs in
  hardware. Each permutation moves 200 bytes of state in and out.

Only `ERRCODE=3` triggers the fallback. Any other failure is returned to the
caller. SHAKE is a public function, so falling back to software framing does
not weaken confidentiality; a software fallback for the entropy source would,
which is why that one is not allowed.

## Simulated transports

The Verilator and AXI transports implement `NTT_FWD`, `NTT_INV` and
`KECCAK_F1600`. Every other mode sets `STATUS.ERR` with `ERRCODE=3`, which
surfaces as `PQC_ERR_UNSUPPORTED`. There is no software fallback in these
transports. The full ML-KEM and ML-DSA cores (`hardware/rtl/mlkem`,
`hardware/rtl/mldsa`) are not reachable through them.

## Consistency tests

The stub and the simulated RTL must return identical bytes through the same
register interface. `tests/unit/test_accel.c` (direct) and
`tests/unit/test_accel_axi.c` (over AXI transactions) check both NTT directions
and the SHA3/SHAKE path, the latter against OpenSSL. A hardware transport would
be held to the same tests.

See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for how this layer fits
into the module.
