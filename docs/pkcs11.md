**English** · [中文](pkcs11.zh-CN.md)

# PKCS#11 interface

The module builds as `pqchsm-pkcs11.dylib` / `.so`, a PKCS#11 v3.2 provider exposing
ML-KEM and ML-DSA through the mechanisms defined natively in that revision.

```bash
cmake --build build --target pqchsm-p11
```

## Obtaining the function list

Two entry points are available and both work:

```c
CK_FUNCTION_LIST_PTR f;
C_GetFunctionList(&f);                       /* 2.40-shaped table */

CK_INTERFACE_PTR iface;
C_GetInterface(NULL, NULL, &iface, 0);       /* "PKCS 11" v3.2 table */
CK_FUNCTION_LIST_3_2_PTR f32 = iface->pFunctionList;
```

`CK_FUNCTION_LIST` is the 2.40 structure and has no fields for the functions introduced
in v3.x. **`C_EncapsulateKey` and `C_DecapsulateKey` are only reachable through
`C_GetInterface`.** Both tables point at the same implementations.

`C_GetInterface` accepts `NULL` or `"PKCS 11"` as the interface name. Requesting
`CKF_INTERFACE_FORK_SAFE` returns `CKR_FUNCTION_FAILED`: the module is not fork-safe and
declines to claim otherwise.

## Mechanisms

| Mechanism | Code | Operations |
|---|---|---|
| `CKM_ML_KEM_KEY_PAIR_GEN` | 0x16 | key pair generation |
| `CKM_ML_KEM` | 0x17 | encapsulate, decapsulate |
| `CKM_ML_DSA_KEY_PAIR_GEN` | 0x1C | key pair generation |
| `CKM_ML_DSA` | 0x1D | sign, verify |

Parameter sets are selected with `CKA_PARAMETER_SET`:

| Attribute value | Algorithm |
|---|---|
| `CKP_ML_KEM_512` / `CKP_ML_KEM_768` / `CKP_ML_KEM_1024` | ML-KEM |
| `CKP_ML_DSA_44` / `CKP_ML_DSA_65` / `CKP_ML_DSA_87` | ML-DSA |

`CKM_HASH_ML_DSA_*` is not implemented.

## Implemented functions

General purpose, slot and token management, sessions, objects, signing, verification,
key pair generation, and KEM operations:

```
C_Initialize          C_Finalize            C_GetInfo             C_GetFunctionList
C_GetInterface        C_GetInterfaceList    C_GetSlotList         C_GetSlotInfo
C_GetTokenInfo        C_GetMechanismList    C_GetMechanismInfo    C_InitToken
C_InitPIN             C_OpenSession         C_CloseSession        C_CloseAllSessions
C_GetSessionInfo      C_Login               C_Logout              C_CreateObject
C_DestroyObject       C_GetAttributeValue   C_FindObjectsInit     C_FindObjects
C_FindObjectsFinal    C_GenerateKeyPair     C_SignInit            C_Sign
C_SignUpdate          C_SignFinal           C_VerifyInit          C_Verify
C_VerifyUpdate        C_VerifyFinal         C_EncapsulateKey      C_DecapsulateKey
```

Unimplemented functions are left as `NULL` entries in the function list rather than
stubs that return an error, so callers can detect capability by inspection.

## Object model

Each slot holds at most one key pair, so a loaded slot presents exactly two objects: a
private key and the corresponding public key. Their handles are derivable from each
other.

`CKA_VALUE` on a public key returns the encoded public key. `CKA_VALUE` on a private key
returns `CKR_ATTRIBUTE_SENSITIVE` unconditionally — there is no code path that exports
private key material.

`C_FindObjectsInit` filters on `CKA_CLASS`; other attributes in the template are
ignored. A session is bound to one slot, so a search returns only that slot's objects.

### Vendor attributes

PKCS#11 has no standard attribute expressing "this key may be included in a backup".
`CKA_EXTRACTABLE` means something different — whether the key can be exported in the
clear, which this module never permits. Two vendor-defined attributes are used instead:

| Attribute | Default | Meaning |
|---|---|---|
| `CKA_PQCHSM_BACKUPABLE` (`CKA_VENDOR_DEFINED \| 0x01`) | `CK_TRUE` | key may be included in M-of-N backups |
| `CKA_PQCHSM_SEED_STORAGE` (`CKA_VENDOR_DEFINED \| 0x02`) | `CK_FALSE` | store the seed and expand on demand instead of storing the expanded key |

The default for `CKA_BACKUPABLE` is true because a token whose keys can never be backed
up makes the entire recovery mechanism inapplicable. Set it to `CK_FALSE` explicitly for
keys that should disappear with the device, such as a device identity key.

## Key import

`C_CreateObject` accepts:

- **Private key from a seed** — `CKA_CLASS = CKO_PRIVATE_KEY`, `CKA_KEY_TYPE`,
  `CKA_PARAMETER_SET`, and `CKA_SEED` (64 bytes for ML-KEM, 32 for ML-DSA).
- **Session secret key** — `CKA_CLASS = CKO_SECRET_KEY` with `CKA_VALUE`.

Supplying `CKA_VALUE` for a private key returns `CKR_ATTRIBUTE_TYPE_INVALID`. There is no
path that loads an expanded private key into a slot; keys enter either by internal
generation or by seed. Since FIPS 203 and 204 keys are fully determined by their seed
this does not reduce capability, and it moves fewer sensitive bytes. For provisioning
without exposing the seed, use the key injection protocol described in
[architecture.md](architecture.md#key-injection).

## Multi-part signing

`C_SignUpdate` accumulates the message and `C_SignFinal` signs it in one operation.
ML-DSA is not a hash-then-sign construction: FIPS 204 computes `μ` over the whole
message with a domain separator and context prefix, and the rejection-sampling loop
needs `μ` on every retry, so the message cannot be consumed incrementally. Memory use is
therefore proportional to message length. Streaming a digest instead requires
`CKM_HASH_ML_DSA_*`, which is not implemented.

`C_VerifyUpdate` / `C_VerifyFinal` behave symmetrically.

## KEM operations

```c
CK_MECHANISM m = { CKM_ML_KEM, NULL, 0 };
CK_ULONG ctlen = sizeof(ct);
CK_OBJECT_HANDLE shared;

f32->C_EncapsulateKey(session, &m, publicKey, tmpl, n, ct, &ctlen, &shared);
f32->C_DecapsulateKey(session, &m, privateKey, tmpl, n, ct, ctlen, &shared);
```

Both produce a **session object** holding the shared secret, not raw bytes, which is
what the specification prescribes. The object is not persisted and is destroyed when the
session closes.

By default the shared secret is `CKA_SENSITIVE` and not `CKA_EXTRACTABLE`, so reading
`CKA_VALUE` returns `CKR_ATTRIBUTE_SENSITIVE`. To read it — for example to demonstrate
that both sides agree — set `CKA_EXTRACTABLE = CK_TRUE` and `CKA_SENSITIVE = CK_FALSE`
in the template.

Passing a ciphertext of the wrong length returns `CKR_ENCRYPTED_DATA_LEN_RANGE`. A
corrupted ciphertext does **not** produce an error: ML-KEM's implicit rejection yields a
deterministic but different shared secret.

## Configuration

The module keeps state in a keystore file, because each PKCS#11 client may be a separate
process:

| Variable | Default | Meaning |
|---|---|---|
| `PQCHSM_KEYSTORE` | `$HOME/.pqchsm/keystore.bin` | keystore path |
| `PQCHSM_SLOTS` | 4 | number of slots |

`C_Initialize` loads the keystore and every state-changing operation writes it back
immediately.

## Behavioural notes

- `C_InitToken` on an already-initialised token returns `CKR_ACTION_PROHIBITED`. Some
  implementations re-initialise; that destroys all contents, so an explicit zeroize
  through the admin tool is required first.
- `C_CloseAllSessions` closes sessions on the given slot only.
- `pLabel` for `C_InitToken` must be exactly 32 bytes, space-padded and not
  NUL-terminated, as the specification requires.
- Failed SO PIN attempts increment a counter but do not lock the slot; locking it would
  render the device unusable, and M-of-N recovery is the intended fallback.

## Client support

Higher-level provider frameworks do not yet support these mechanisms. Use a low-level
binding that passes mechanism codes directly — PyKCS11 in Python, the JDK FFM API or a
wrapper library in Java. [demo/README.md](../demo/README.md) documents the specific
gaps and provides working examples for both.
