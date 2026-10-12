**English** · [中文](README.zh-CN.md)

# Hardware abstraction layer

This directory implements the seam between the cryptographic core and whatever actually
performs the arithmetic. Everything above it — slot manager, keystore, wrapping, backup,
audit, PKCS#11 — is written against handles and never learns which backend is in use.

## Two levels of abstraction

**`pqc_backend_t`** (declared in `include/pqchsm/pqc.h`) is the vtable for
keygen/encaps/decaps/sign/verify. `src/crypto/pqc_native.c` implements it in software
(vendored mlkem-native / mldsa-native, the same source tree the OP-TEE TA uses);
`pqc_accel.c` here implements the same vtable on top of a register interface, and
`pqc_sdfe.c` implements it by calling the FPGA through the `pqchsm_fpgad` service.

**`accel_transport_t`** (declared in `include/pqchsm/accel.h`) is that register
interface: an AXI-style map of `CTRL`, `STATUS`, `MODE`, `PARAM`, `IN_LEN`, `OUT_LEN`
and `ERRCODE`, with bulk data moved separately because a real design would use AXI-DMA
rather than register reads. The map was fixed before any RTL was written, so interface
mistakes surface in software rather than during board bring-up.

## Files

| File | Role |
|---|---|
| `pqc_accel.c` | `pqc_backend_t` over the register interface; also `accel_ntt()`, `accel_keccak_f1600()`, `accel_shake()` |
| `accel_stub.c` | Software transport. Implements the register semantics exactly, calling the software backend (`pqc_backend_native()`) internally. Always available. |
| `accel_verilator.c` | Transport driving Verilator-simulated RTL. Compiled only when Verilator is present; otherwise `accel_transport_verilator()` returns `NULL`. |
| `accel_axi.c` | Transport driving the simulated RTL through real AXI4-Lite register transactions and AXI4-Stream packets (the `docs/REGISTERS.md` contract). Compiled only when Verilator is present; otherwise `accel_transport_axi()` returns `NULL`. |
| `verilator/ntt_sim.cpp` | C wrapper around the Verilated `ntt_core` |
| `verilator/keccak_sim.cpp` | C wrapper around the Verilated `keccak_f1600` |
| `verilator/axi_sim.cpp` | C wrapper around the Verilated AXI-fronted accelerator, used by `accel_axi.c` |
| `accel_mmap.c` | `/dev/mem` + `mmap` transport for real programmable logic. Returns `NULL` unless `PQCHSM_ACCEL_MMAP_BASE` and `PQCHSM_ACCEL_MMAP_BUF` are supplied at build time. Not yet driven against real PL. |
| `hwrng.c` | Driver for the PL entropy source (`trng_axi`), following the contract in `docs/REGISTERS.md` |
| `hwrng_stub.c` | Software model of `trng_axi`'s register semantics. FIFO filled by OpenSSL — this models the *interface*, not entropy. |
| `hwrng_mmap.c` | `/dev/mem` + `mmap` transport for the PL TRNG. Returns `NULL` unless `PQCHSM_HWRNG_MMAP_BASE` is supplied at build time; with the delivered `SECURE_ONLY` configuration the normal world cannot reach the registers, so this is for debug bitstreams and as a TA-driver prototype |
| `hwrng_sdfe.c` | Byte-level entropy source (`hwrng_byte_source_sdfe()`) that fetches TRNG bytes through `libsdfe` → `pqchsm_fpgad`; the path that works in the delivered form |
| `pqc_sdfe.c` | `pqc_backend_t` (`pqc_backend_sdfe()`) that runs ML-KEM / ML-DSA on the FPGA via the service. Byte-form keygen/decaps/sign are deliberately not implemented: private keys stay in hardware and are used through handles |
| `sdfe_conn.c`, `sdfe_conn.h` | The single shared connection to the service (local socket, or mTLS TCP when `PQCHSM_SDFE_HOST` + `PQCHSM_SDFE_PKI` are set), used by both `hwrng_sdfe.c` and `pqc_sdfe.c`; the daemon serves one connection at a time |
| `hwrng_stub.h` | Internal fault-injection hooks for the software TRNG model (tests only) |

## The entropy source is a separate peripheral

`hwrng.h` is a second transport seam, deliberately not folded into `accel.h`'s opcode
space. Three architectural reasons, the same ones that keep them separate in the RTL:
their lifecycles differ (the accelerator is command/complete, the TRNG free-runs), their
access policies differ (entropy is usually guarded more tightly than algorithm cores),
and their fault domains differ (a TRNG alarm must report independently, not queue behind
the accelerator's busy state).

Once a hardware entropy source (register transport or byte source) is installed,
`pqc_random_bytes()` draws from it and **does not fall back to software on failure**.
The software stub reports `is_hardware = 0`, so installing it does not change where
`pqc_random_bytes()` gets its bytes (OpenSSL). Silent fallback would make
"the entropy comes from hardware" quietly false at exactly the moment it matters.

## Division of labour between hardware and software

`accel_shake()` has two paths and prefers the hardware sponge:

- **Mode 10 (`ACCEL_MODE_SHAKE`)** — the whole sponge runs inside the PL; only the
  message and the digest cross the bus. This is the default, taken whenever the message
  and the output both fit in `ACCEL_SHAKE_MAX` (512 bytes, the size of the PL-side
  buffer). Rate, domain-separation suffix and output length are packed into `PARAM`
  (see `ACCEL_SHAKE_PARAM`).
- **Mode 9 (`ACCEL_MODE_KECCAK_F1600`)** — when the message exceeds that limit, or the
  transport does not implement mode 10 (it answers `ERRCODE=3`), framing falls back to C
  and only the permutation goes to hardware.

**Why the first path matters** is not speed but that the sponge's intermediate state
never leaves the crypto boundary. Mode 9 ships 200 bytes of state in and out for every
permutation, and in ML-KEM that intermediate state *is* the context of the ρ/σ expansion.

**This fallback is not the same thing as the entropy source's "never fall back".** SHAKE
is a public function: falling back to software framing costs no confidentiality
guarantee, only that extra layer of depth. Falling back to a software RNG would change
the security root itself. That difference is why one is allowed and the other fails hard.

**The fallback triggers on `ERRCODE=3` only** (mode not implemented). Any other failure
is reported upward as-is — an accelerator that is broken but always caught by software is
an accelerator nobody will ever notice is broken.

## What the simulated backend covers

The Verilator and AXI transports implement `NTT_FWD`, `NTT_INV` and `KECCAK_F1600`.
Every other mode sets `STATUS.ERR` with `ERRCODE=3` and surfaces as
`PQC_ERR_UNSUPPORTED`. They deliberately do **not** fall back to software: a silent
fallback would make "the RTL path works" an illusion. The full ML-KEM/ML-DSA cores
(`hardware/rtl/mlkem`, `hardware/rtl/mldsa`) are not reachable through these simulated
transports; on the board they are used through `pqc_sdfe.c`.

## Consistency requirement

The stub and the simulated RTL must produce byte-identical results through the same
register interface. `tests/unit/test_accel.c` (direct) and `tests/unit/test_accel_axi.c`
(over AXI transactions) assert this for both NTT directions and for the whole SHA3/SHAKE
path, and cross-check the latter against OpenSSL. The same
requirement will apply to a real hardware transport.

See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for how this layer fits into the
rest of the module.
