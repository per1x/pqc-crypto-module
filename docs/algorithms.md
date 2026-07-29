**English** · [中文](algorithms.zh-CN.md)

# Algorithm inventory

Every cryptographic algorithm the module uses, where it is implemented, and what
its correctness is pinned to. The table is the starting point for a validation
submission under FIPS 140-3 or GM/T 0028: each row is an algorithm that would
need its own algorithm certificate, and the evidence column names the artefact
that supports it.

## Approved algorithms

| Algorithm | Standard | Parameter sets | Use in the module | Implementation | Evidence |
|---|---|---|---|---|---|
| ML-KEM | FIPS 203 | 512, 768, 1024 | Key encapsulation; key injection; backup transport | liboqs (`src/crypto/pqc_liboqs.c`) | 180 ACVP vectors, byte-exact |
| ML-DSA | FIPS 204 | 44, 65, 87 | Signature generation and verification; audit-chain anchoring; device identity | liboqs (`src/crypto/pqc_liboqs.c`) | 210 ACVP vectors, byte-exact |
| AES-256-GCM | FIPS 197, SP 800-38D | 256-bit key, 96-bit nonce, 128-bit tag | Key wrapping at rest; keystore records | OpenSSL 3 (`src/store/wrap.c`) | OpenSSL validated implementation; round-trip and tamper tests |
| SHA3-256 | FIPS 202 | — | Audit hash chain; Shamir share checksums | OpenSSL 3 (`src/crypto/kdf.c`, `src/audit/audit.c`) | Compared against OpenSSL and an independent Keccak permutation |
| SHAKE128 / SHAKE256 | FIPS 202 | — | XOF inside ML-KEM and ML-DSA; sponge built on the RTL Keccak core | OpenSSL 3; RTL path in `src/hal/pqc_accel.c` | Byte-exact against OpenSSL and `hashlib` |
| KMAC256 | SP 800-185 | 256-bit key | Key derivation (KEK, BEK); whole-file keystore MAC | OpenSSL 3 (`src/crypto/kdf.c`) | Compared against the NIST sample values and OpenSSL |

Each approved algorithm additionally has a pre-operational known-answer test whose
expected value comes from outside the module; see
[security-policy.md](security-policy.md#10-self-tests).

## Non-approved but allowed

| Function | Standard | Use | Implementation | Note |
|---|---|---|---|---|
| Shamir secret sharing over GF(256) | — | M-of-N backup and recovery | `src/backup/shamir.c` | Information-theoretic; the GF(256) multiply is constant-time. Share checksums are unkeyed and detect corruption, not tampering. |
| Random bit generation | — | Seeds, nonces, recovery master key | `RAND_bytes` (OpenSSL) | The OpenSSL DRBG. A hardware noise source with SP 800-90B health tests exists in RTL (`hardware/rtl/trng/`) but is not wired into the software path. |

## Hardware implementations

The RTL cores are arithmetic building blocks, not complete algorithm
implementations. They are listed separately because their validation status
differs: they are verified in simulation against independent oracles and against
NIST vectors reconstructed from those same operators, but no hardware
implementation of a complete algorithm exists.

| Core | Role in the algorithm | Verification |
|---|---|---|
| `ntt_core`, `mlkem_basemul` | ML-KEM transform domain | Schoolbook negacyclic convolution; ACVP `ek`/`dk` reconstruction |
| `mlkem_compress`, `mlkem_decompress` | FIPS 203 §4.2.1 | Exhaustive over the whole input domain against the rational-arithmetic definition |
| `mlkem_cbd2`, `mlkem_cbd3` | FIPS 203 Alg 8 | FIPS 203 bit-counting definition, bit groups exhaustive |
| `mlkem_rej_pair`, `mlkem_rej_uniform` | FIPS 203 Alg 7 | Independent `SampleNTT` over a real SHAKE128 stream |
| `mlkem_encode12`, `mlkem_decode12` | FIPS 203 ByteEncode/Decode | Round-trip and ACVP reconstruction |
| `mldsa_ntt_core`, `mldsa_mont_reduce` | ML-DSA transform domain | Schoolbook negacyclic convolution; ACVP `pk`/`sk` reconstruction |
| `mldsa_power2round`, `mldsa_decompose` | FIPS 204 §7 | Defining decompositions and value ranges |
| `mldsa_make_hint`, `mldsa_use_hint` | FIPS 204 §7 | The recovery property FIPS 204 depends on |
| `mldsa_rej_uniform`, `mldsa_rej_eta` | FIPS 204 Alg 30, 31 | Independent implementations; acceptance thresholds value by value |
| `keccak_f1600` | FIPS 202 permutation | Published all-zero permutation output; sponge against OpenSSL |
| `trng_health` | SP 800-90B §4.4 | Cutoffs recomputed from the definition; stuck-at, biased, and uniform sources |

## Key and parameter inventory

| Item | Type | Generation | Storage | Zeroisation |
|---|---|---|---|---|
| KDR | 256-bit root secret | Device-bound (currently a fixed constant — see limitations) | Never leaves the module | Not applicable while it is a constant |
| KEK | 256-bit AES key | KMAC256 from KDR | Derived on demand, never stored | Zeroised after each use |
| BEK | 256-bit AES key | KMAC256 from the recovery master key | Derived on demand | Zeroised after each use |
| Recovery master key | 256-bit secret | `RAND_bytes` | Split into M-of-N shares, never stored whole | Zeroised after splitting |
| Slot private keys | ML-KEM / ML-DSA private keys | On-device generation, or injection under an encapsulated session key | AES-256-GCM wrapped under the KEK | Zeroised on slot destruction |
| Device identity key | ML-DSA private key | On-device generation | Wrapped like any slot key | Zeroised on device reset |
| PINs | User and SO authentication data | Set by the operator | KMAC256 digest with a per-slot salt | Plaintext buffers zeroised after comparison |

## Limitations relevant to validation

The following would each block a validation submission and are stated here so
that the inventory is not read as a claim of readiness.

- The key derivation root is a fixed constant, so no key is genuinely
  device-bound. A real device sources it from eFUSE, BBRAM, or a PUF.
- Random bits come from the OpenSSL DRBG seeded by the operating system, not
  from a noise source inside the module boundary.
- The module boundary is a process address space. Plaintext key material exists
  in that address space during operations.
- No algorithm certificates have been obtained. The ACVP vectors are run
  locally against the vendor implementation; that is evidence of correctness,
  not a certificate.
