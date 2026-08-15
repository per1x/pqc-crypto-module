**English** · [中文](API.zh-CN.md)

# API

Two front ends are provided, over the same hardware: an **SDF-style C library**
(`service/`) that speaks to the FPGA cores through the secure world, and a
**PKCS#11 v3.2 module** (`src/p11/`) over the host software stack.

- [Layering](#layering)
- [SDF-style interface](#sdf-style-interface)
- [Mapping to GM/T 0018](#mapping-to-gmt-0018)
- [PKCS#11 v3.2](#pkcs11-v32)
- [Where private keys live](#where-private-keys-live)

## Layering

```
┌──────────────────────────────────────────────────────────────┐
│ application        business code, standard interfaces only   │
├──────────────────────────────────────────────────────────────┤
│ libsdfe / pqchsm-pkcs11.so   standard API → internal wire    │
│                              stateless, safe across processes│
├──────────────────────────────────────────────────────────────┤
│ pqchsm_fpgad       sessions, handles, ACL, serialisation     │
├──────────────────────────────────────────────────────────────┤
│ /dev/secmmio       misc device turning requests into SMCs.   │
│                    NOT a trust boundary — a courier          │
├──────────────────────────────────────────────────────────────┤
│ BL31 SiP (EL3)     whitelist, and the only thing that can    │
│ AxPROT[1] = 0      issue a secure transaction  ◀── boundary  │
├──────────────────────────────────────────────────────────────┤
│ FPGA cores         AXI firewall → trng / key_vault / sym /   │
│ SECURE_ONLY=1      mlkem. Non-secure access refused at the bus│
└──────────────────────────────────────────────────────────────┘
```

Why each layer has to exist:

| Layer | Problem it solves | Without it |
|---|---|---|
| Standard library | Applications swap modules without code changes | Every application assembles the wire protocol itself |
| Daemon | Multiple processes share **one** piece of hardware: sessions, handles, serialisation | Two processes drive `mlkem_axi` at once and trample each other's registers |
| Kernel node | User space cannot issue an SMC | The command logic would have to move into the kernel |
| EL3 SiP | Normal-world transactions always carry `AxPROT[1] = 1` and are refused | The cores are simply unusable in a `SECURE_ONLY=1` build |
| PL firewall | The gate itself | No boundary — and nothing on the PS side can stand in for it |

The daemon is the layer most often assumed to be droppable. It is not: all three
cores are stateful sequences — write registers, start, poll, read result — and
two processes interleaving on one core produce results that are **mutually
misaligned but individually plausible**. Serialisation needs a single point, and
that point has to remember sessions.

## SDF-style interface

`service/sdfe.h`. The prefix is `SDFE_` ("SDF Extended"), not `SDF_`: the naming
follows GM/T 0018 convention, but the Chinese standards have not yet defined an
ML-KEM interface, so these do not impersonate standard function names.

**The library performs no cryptography.** It translates a call into one request
to `pqchsm_fpgad`. It is stateless; separate processes can each link it, each
open sessions, and never see each other's handles.

```c
/* device and session */
int SDFE_OpenDevice(SDFE_HANDLE *phDev);
int SDFE_CloseDevice(SDFE_HANDLE hDev);
int SDFE_OpenSession(SDFE_HANDLE hDev, SDFE_HANDLE *phSession);
int SDFE_CloseSession(SDFE_HANDLE hSession);
int SDFE_GetDeviceInfo(SDFE_HANDLE hSession, char *buf, size_t cap);

/* random — from the PL ring-oscillator source, not a software PRNG */
int SDFE_GenerateRandom(SDFE_HANDLE hSession, uint32_t len, uint8_t *out);

/* ML-KEM — dk stays with the daemon; the application gets a handle */
int SDFE_GenerateKeyPair_MLKEM(SDFE_HANDLE hSession, uint32_t pset,
                               uint8_t *ek, uint32_t *ek_len,
                               uint32_t *key_handle);
int SDFE_Encapsulate_MLKEM(SDFE_HANDLE hSession, uint32_t pset,
                           const uint8_t *ek, uint32_t ek_len,
                           uint8_t *ss, uint32_t *ss_len,
                           uint8_t *ct, uint32_t *ct_len);
int SDFE_Decapsulate_MLKEM(SDFE_HANDLE hSession, uint32_t key_handle,
                           const uint8_t *ct, uint32_t ct_len,
                           uint8_t *ss, uint32_t *ss_len);

/* symmetric — the key goes into key_vault and is thereafter used by slot */
int SDFE_ImportKey(SDFE_HANDLE hSession, uint32_t slot,
                   const uint8_t *key, uint32_t key_len);
int SDFE_Encrypt(SDFE_HANDLE hSession, uint32_t alg, uint32_t slot,
                 const uint8_t *in, uint8_t *out);   /* one 16-byte block */
int SDFE_Decrypt(SDFE_HANDLE hSession, uint32_t alg, uint32_t slot,
                 const uint8_t *in, uint8_t *out);

/* public-key encryption of arbitrary data (KEM-DEM): ML-KEM.Encaps (hardware,
 * dk on-chip) wraps a shared secret; AES-256-GCM (software) is the authenticated
 * DEM. blob = ct ‖ iv ‖ tag ‖ ciphertext. See service/sdfe_pkenc.h. */
int SDFE_PKEncrypt(SDFE_HANDLE hSession, uint32_t pset,
                   const uint8_t *ek, uint32_t ek_len,
                   const uint8_t *data, uint32_t data_len,
                   uint8_t *out, uint32_t *out_len);
int SDFE_PKDecrypt(SDFE_HANDLE hSession, uint32_t key_handle,
                   const uint8_t *blob, uint32_t blob_len,
                   uint8_t *data, uint32_t *data_len);   /* auth fail → no plaintext */

const char *SDFE_StrError(int rv);
```

Return codes follow SDF convention: `SDR_OK` (0) on success, `SDR_BASE + n`
otherwise — `SDR_OPENDEVICE`, `SDR_COMMFAIL`, `SDR_INARGERR`,
`SDR_KEYNOTEXIST`, `SDR_HARDFAIL`, `SDR_UNKNOWERR`.

Parameter sets are `SDFE_MLKEM_512` / `_768` / `_1024`; symmetric algorithms are
`SDFE_ALG_AES128` / `_AES256` / `_SM4`, matching the `ALG` field of `sym_axi`.

`service/sdf_demo.c` is a worked example that contains no hardware detail at
all — no registers, no `/dev/mem`, no SMC — and links **only** `libsdfe`, no
crypto library. It cannot compute anything; every correct answer it prints came
from the FPGA. See [USAGE.md](USAGE.md#running-the-sdf-demo).

## Mapping to GM/T 0018

| SDF function | Maps to | Status |
|---|---|---|
| `SDF_OpenDevice` / `CloseDevice` | Connect/disconnect the daemon socket | Available |
| `SDF_OpenSession` / `CloseSession` | `CMD_SESSION_OPEN` / `_CLOSE` | Implemented |
| `SDF_GetDeviceInfo` | Static capability table + slot info | Available |
| `SDF_GenerateRandom` | `trng_axi` `RDATA`, via EL3 | **Hardware** |
| `SDF_Encrypt` / `SDF_Decrypt` (SM4) | `sym_axi` + `key_vault` slot | **Hardware** |
| `SDF_HashInit/Update/Final` (SM3) | `sym_axi` SM3 path | **Hardware** |
| `SDF_GenerateKeyPair_ECC` (SM2) | — | ❌ **No SM2 core** |
| `SDF_InternalSign_ECC` / `InternalVerify_ECC` | — | ❌ No SM2 core |
| `SDF_ImportKeyWithISK_ECC` and the key-exchange family | Depends on SM2 | ❌ No SM2 core |
| `SDF_ExportSignPublicKey_ECC` | Public keys may leave; private keys never | Structurally sound |

SDF's notion of an *internal key* maps naturally onto `key_vault`:
`SDF_InternalSign` takes a device-internal key index (`uiISKIndex`) and never
carries a private key, which is exactly "referenced by slot or handle, never
leaves the hardware".

To be clear about the gap: this design covers SDF's **symmetric, hash and random
number** services. The asymmetric family (SM2/ECC) has **no hardware** here, and
for a Chinese validation submission that is a substantial missing piece.

**ML-KEM has no Chinese standard interface yet**, so there are two routes. This
project treats **PKCS#11 v3.2's PQC mechanisms as the primary path** — the
international standard is settled, the headers are vendored, and
`C_Encapsulate`/`C_DecapsulateKey` are implemented — and keeps the `SDFE_*`
names as a thin wrapper onto the same wire commands, so that when a Chinese
standard does land, the change is confined to that layer.

## PKCS#11 v3.2

`src/p11/p11_module.c` implements 40 `C_` functions (36 in the 2.40
`CK_FUNCTION_LIST`, plus `C_GetInterfaceList` / `C_GetInterface` /
`C_EncapsulateKey` / `C_DecapsulateKey` in the 3.2 table). Build it with
`cmake --build build --target pqchsm-p11`.

```c
CK_INTERFACE *iface;
C_GetInterface(NULL, NULL, &iface, 0);      /* v3.2 mechanisms live here */
```

| Mechanism | Backed by | Status |
|---|---|---|
| `CKM_ML_KEM` (0x17) | `mlkem_axi`, all three parameter sets | **Hardware** |
| `CKM_AES_GCM` | AEAD DEM over the KEM shared secret (`C_Encrypt`/`C_Decrypt`) | Software GCM; KEM half in hardware |
| `CKM_AES_ECB` / `CKM_AES_CBC` | `sym_axi` AES-128/256 | **Hardware** |
| `CKM_SHA3_256` | `sha3_core` (currently reached only through ML-KEM) | Needs a dedicated path |
| `CKM_ML_DSA` (0x1D) | ML-DSA operators, not chained into cores | ❌ Software only |
| SM4 / SM3 | No standard mechanism code exists | Vendor-defined codes |

`C_GenerateKeyPair` → `CMD_GENERATE` → `mlkem_axi` KeyGen;
`C_DecapsulateKey` → `CMD_DECAPS` → `mlkem_axi` Decaps.

The object model follows PKCS#11 closely, because a one-to-one mapping avoids an
impedance layer. Each slot holds at most one key pair and therefore presents
exactly two objects whose handles are derivable from one another; no separate
object table is needed. Access control is role-based (`SO`, `USER`) and enforced
in the slot manager rather than in the PKCS#11 layer, so the CLI and the daemon
get identical enforcement.

Known deviations: `CKM_HASH_ML_DSA_*` is not implemented, so multi-part signing
buffers the whole message instead of streaming a digest. Higher-level provider
frameworks (JCA, PyCryptodome and friends) do not yet support these mechanisms;
the demos in `demo/` therefore use the low-level binding directly, and
`demo/java/SunP11Probe.java` demonstrates why.

## Where private keys live

Two cases that must not be collapsed into one sentence.

| Key | Where it lives | Leaves the hardware? |
|---|---|---|
| Symmetric (AES / SM4) | `key_vault` register array | **No.** The RTL has no bus-side read path. The demo wipes its own copy after import, and from then on nobody — including the daemon — can retrieve it |
| ML-KEM `dk` | Returned by `KeyGen` today; held by the daemon | ⚠️ **Yes, it leaves the hardware** — but not the *interface*. The application only ever holds a handle |

The second row is the current state and cannot be shortened to "private keys
never leave the hardware". Making that sentence true requires private-key
storage and handle-based use inside `mlkem_axi`, which is product work, not
prototype work. See [SECURITY.md](SECURITY.md#limitations).
