# Architecture

This document describes how the module is structured and why the boundaries fall where
they do. For build and usage instructions see the [README](../README.md); for the
PKCS#11 interface see [pkcs11.md](pkcs11.md).

## Layering

```
   PKCS#11 v3.2 shared library          Daemon, CLI, admin tool
   src/p11/                             cli/
 ─────────────────────────────────────────────────────────────────
   slot manager     keystore        backup / recovery    audit
   src/slot/        src/store/      src/backup/          src/audit/
 ─────────────────────────────────────────────────────────────────
   pqc_backend_t          include/pqchsm/pqc.h
 ─────────────────────────────────────────────────────────────────
   accel_transport_t      include/pqchsm/accel.h
   stub | Verilator RTL | mmap (future)
```

Each layer depends only on the interface below it. The upper layers are written against
opaque handles, so the backend can change without any caller changing.

## Handles and the API surface

No function declared in `include/pqchsm/` returns private key material. Key generation
returns an `hsm_handle_t`; signing takes a handle and a message; decapsulation takes a
handle and a ciphertext. Public keys may cross the boundary, private keys never do.

A handle encodes the slot identifier and the slot's generation counter. Destroying an
object increments the generation, so every previously issued handle for that slot
becomes invalid immediately rather than silently referring to a new key.

## Slot model

The slot/token/object/session model follows PKCS#11 closely, because the module is
intended to be used through PKCS#11 and a one-to-one mapping avoids an impedance layer.

Each slot holds at most one key pair, in keeping with a per-slot storage budget suitable
for an embedded target. A loaded slot therefore presents exactly two PKCS#11 objects,
whose handles are derivable from each other; no separate object table is needed.

Slot state is an explicit transition table rather than scattered conditionals:

```
UNINIT ──InitToken──► INITIALIZED ──Generate/LoadSeed──► LOADED ──InUse──► IN_USE
   ▲                       │                               │                 │
   └────────────── Zeroize (reachable from any state, irreversible) ──────────┘
```

Access control is role-based (`SO`, `USER`) and enforced at the slot manager, not at the
PKCS#11 layer, so the CLI and the daemon get the same enforcement.

## Key hierarchy

```
KDR  (device-bound root, 32 bytes)
 └── KEK        wraps key material at rest; cannot leave the device
RMK  (recovery master key, randomly generated)
 └── BEK        wraps backup blobs; portable to a replacement device
      └── Shamir M-of-N shares over GF(256)
```

Two distinct wrapping keys exist because the two purposes conflict. Material wrapped
under a device-bound key is safe but unrecoverable if the device fails; material
wrapped under a portable key is recoverable but is only as protected as the share
custody. Each slot carries a policy bit controlling whether its key participates in
backup at all, so a device identity key can be made deliberately unrecoverable.

Shamir splitting uses a constant-time GF(256) multiply. Share integrity is checked with
an unkeyed hash prefix, which detects corruption; the authoritative integrity check is
the AES-GCM tag after reassembly.

Each slot also stores a randomly generated PIN verification key inside its wrapped
blob, rather than deriving the verifier from the device root. Deriving it from the root
would make a restored token unable to authenticate on a replacement device.

## Keystore format

A keystore file is a sequence of per-slot blobs plus a whole-file MAC:

```
slot blob := plaintext metadata ‖ AES-256-GCM(KEK, aad = metadata, key material ‖ PIN material)
file      := header ‖ slot blob × N ‖ KMAC256(file)
```

Metadata is authenticated as additional data rather than encrypted, so a slot's
algorithm, usage and policy can be read without unwrapping, while remaining
tamper-evident.

Writes are atomic: `write to temporary file → fsync → rename → fsync(directory)`. A
crash leaves either the previous file or the new one, never a partial one.

## Audit chain

Each state transition appends a record whose SHA3-256 hash covers the previous record's
hash. This makes any modification propagate forward and be detectable — provided a
reference point exists.

A pure hash chain does not survive an attacker who can rewrite the entire file, since
they can recompute every hash. The chain head is therefore signed with an ML-DSA device
identity key and anchored outside the device. Verification compares the recomputed head
against the last anchored signature.

The module assumes a single writer and does not lock the audit file.

## Hardware abstraction

`include/pqchsm/accel.h` defines an AXI-style register map:

| Offset | Register | Access | Purpose |
|---|---|---|---|
| 0x00 | `CTRL` | W | `START`, `SOFT_RESET` |
| 0x04 | `STATUS` | R | `DONE`, `BUSY`, `ERR` |
| 0x08 | `MODE` | RW | operation code |
| 0x0C | `PARAM` | RW | parameter set |
| 0x10 | `IN_LEN` | RW | input length |
| 0x14 | `OUT_LEN` | R | output length, written by the accelerator |
| 0x18 | `ERRCODE` | R | error detail |

Bulk data is not moved through registers; the transport exposes separate
`write_data`/`read_data` entry points, corresponding to DMA on real hardware.

Three transports implement this interface identically:

- **Stub** — pure software, calls liboqs. Always available.
- **Verilator** — drives simulated RTL. Implements the NTT and Keccak modes; every
  other mode returns an explicit "unsupported" error rather than falling back to
  software, so the coverage of the RTL path is never overstated.
- **mmap** — `/dev/mem` on real programmable logic. Not yet implemented.

The stub and the simulated RTL are required to produce byte-identical results through
this interface, which is asserted in `tests/unit/test_accel.c`.

### SHA3 and SHAKE

`accel_shake()` implements the sponge — padding, rate, absorb, squeeze — in C, calling
the transport only for the Keccak-f[1600] permutation. A permutation-only core is
smaller and serves SHAKE128, SHAKE256, SHA3-256 and SHA3-512 alike; framing belongs on
the processor side. The result is that the entire SHA3/SHAKE path can be executed
against simulated RTL and compared with OpenSSL.

## Key injection

Provisioning a key without exposing it on the wire uses the module's own KEM:

```
provisioning host                                device
─────────────────                                ──────
obtain device ML-KEM public key  ◄────────────── public key of a KEM slot
(ct, CEK) = Encaps(ek)
blob = header ‖ ct ‖ AES-GCM(CEK, header, seed)
                                 ──────────────► Decaps(dk, ct) recovers CEK
                                                 unwrap seed, load into target slot
```

Only ciphertext travels; both sides zero the session key afterwards. What is injected
is a seed, not an expanded private key: FIPS 203 and 204 keys are fully determined by
their seed, so this moves fewer sensitive bytes and lets the device verify the
expansion itself.

Overwriting an already-loaded slot requires that slot's policy to permit injection, so
provisioning cannot silently displace a key that was not meant to be replaceable.

## Randomness

Random bytes come from OpenSSL's `RAND_bytes`, routed through a single indirection so
that a hardware entropy source can replace it. Deterministic test vectors are handled
by dedicated derandomised entry points (`pqc_keypair_from_seed`, `pqc_encaps_derand`)
rather than by substituting the generator, so the production path is never
reconfigurable at runtime.

## Directory map

| Path | Contents |
|---|---|
| `include/pqchsm/` | Public headers; the entire API surface |
| `src/crypto/` | liboqs binding, KDF, key derivation root |
| `src/slot/` | Slot FSM, sessions, metadata, persistence |
| `src/store/` | AES-256-GCM wrapping, keystore file format |
| `src/backup/` | Shamir splitting, backup and restore, key injection |
| `src/audit/` | Hash chain and ML-DSA anchoring |
| `src/hal/` | Accelerator abstraction and transports |
| `src/p11/` | PKCS#11 v3.2 shared library |
| `src/proto/` | TLV command protocol |
| `src/util/` | Secure zeroing, locked allocation, KAT parsing |
| `cli/` | Daemon, client CLI, admin tool |
| `hardware/` | RTL, testbenches, reference model, synthesis scripts |
| `tools/` | Vector fetching, benchmarks, regression scripts |
