# Hardware abstraction layer

This directory implements the seam between the cryptographic core and whatever actually
performs the arithmetic. Everything above it — slot manager, keystore, wrapping, backup,
audit, PKCS#11 — is written against handles and never learns which backend is in use.

## Two levels of abstraction

**`pqc_backend_t`** (declared in `include/pqchsm/pqc.h`) is the vtable for
keygen/encaps/decaps/sign/verify. `src/crypto/pqc_liboqs.c` implements it directly;
`pqc_accel.c` here implements the same vtable on top of a register interface.

**`accel_transport_t`** (declared in `include/pqchsm/accel.h`) is that register
interface: an AXI-style map of `CTRL`, `STATUS`, `MODE`, `PARAM`, `IN_LEN`, `OUT_LEN`
and `ERRCODE`, with bulk data moved separately because a real design would use AXI-DMA
rather than register reads. The map was fixed before any RTL was written, so interface
mistakes surface in software rather than during board bring-up.

## Files

| File | Role |
|---|---|
| `pqc_accel.c` | `pqc_backend_t` over the register interface; also `accel_ntt()`, `accel_keccak_f1600()`, `accel_shake()` |
| `accel_stub.c` | Software transport. Implements the register semantics exactly, calling liboqs internally. Always available. |
| `accel_verilator.c` | Transport driving Verilator-simulated RTL. Compiled only when Verilator is present; otherwise `accel_transport_verilator()` returns `NULL`. |
| `verilator/ntt_sim.cpp` | C wrapper around the Verilated `ntt_core` |
| `verilator/keccak_sim.cpp` | C wrapper around the Verilated `keccak_f1600` |

A future `accel_mmap.c` will add a `/dev/mem` + `mmap` transport for real programmable
logic. Nothing above this directory changes when it does.

## Division of labour between hardware and software

`accel_shake()` performs the sponge — padding, rate, absorb, squeeze — in C, and calls
the transport only for the Keccak-f[1600] permutation. This mirrors how a real design
would be partitioned: a permutation-only core is smaller and serves SHAKE128, SHAKE256,
SHA3-256 and SHA3-512 alike, with framing handled by the processing system.

## What the simulated backend covers

The Verilator transport implements `NTT_FWD`, `NTT_INV` and `KECCAK_F1600`. Every other
mode sets `STATUS.ERR` with `ERRCODE=3` and surfaces as `PQC_ERR_UNSUPPORTED`. It
deliberately does **not** fall back to software: a silent fallback would make "the RTL
path works" an illusion. Full ML-KEM/ML-DSA cores are future work.

## Consistency requirement

The stub and the simulated RTL must produce byte-identical results through the same
register interface. `tests/unit/test_accel.c` asserts this for both NTT directions and
for the whole SHA3/SHAKE path, and cross-checks the latter against OpenSSL. The same
requirement will apply to a real hardware transport.

See [docs/design/status-and-roadmap.md](../../docs/design/status-and-roadmap.md) for the
broader plan.
