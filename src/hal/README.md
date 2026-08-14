**English** · [中文](README.zh-CN.md)

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
| `accel_mmap.c` | `/dev/mem` + `mmap` transport for real programmable logic. Returns `NULL` unless the physical base addresses are supplied at build time. |
| `hwrng.c` | Driver for the PL entropy source (`trng_axi`), following the contract in `docs/REGISTERS.md` |
| `hwrng_stub.c` | Software model of `trng_axi`'s register semantics. FIFO filled by OpenSSL — this models the *interface*, not entropy. |
| `hwrng_mmap.c` | `/dev/mem` + `mmap` transport for the PL TRNG |

## The entropy source is a separate peripheral

`hwrng.h` is a second transport seam, deliberately not folded into `accel.h`'s opcode
space. Three architectural reasons, the same ones that keep them separate in the RTL:
their lifecycles differ (the accelerator is command/complete, the TRNG free-runs), their
access policies differ (entropy is usually guarded more tightly than algorithm cores),
and their fault domains differ (a TRNG alarm must report independently, not queue behind
the accelerator's busy state).

Once a transport is installed, `pqc_random_bytes()` and liboqs' random source both draw
from it, and **neither falls back to software on failure**. Silent fallback would make
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

The Verilator transport implements `NTT_FWD`, `NTT_INV` and `KECCAK_F1600`. Every other
mode sets `STATUS.ERR` with `ERRCODE=3` and surfaces as `PQC_ERR_UNSUPPORTED`. It
deliberately does **not** fall back to software: a silent fallback would make "the RTL
path works" an illusion. Full ML-KEM/ML-DSA cores are future work.

## Consistency requirement

The stub and the simulated RTL must produce byte-identical results through the same
register interface. `tests/unit/test_accel.c` asserts this for both NTT directions and
for the whole SHA3/SHAKE path, and cross-checks the latter against OpenSSL. The same
requirement will apply to a real hardware transport.

See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for how this layer fits into the
rest of the module.
