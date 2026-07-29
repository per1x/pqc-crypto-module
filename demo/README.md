# PKCS#11 provider demos

Two demonstrations of loading the module as a PKCS#11 provider and driving a complete
key lifecycle from Python and from Java: `C_Initialize` → initialise the token and PINs
→ log in → generate ML-DSA and ML-KEM keys into slots → sign and verify → read
attributes → enumerate objects.

All commands assume the repository root as the working directory.

## Python (PyKCS11, 25 assertions)

```bash
cmake --build build --target pqchsm-p11
python3 -m venv .venv-p11 && ./.venv-p11/bin/pip install -q PyKCS11
./.venv-p11/bin/python demo/python/pqchsm_demo.py
```

## Java (JDK 22+ FFM, no external dependencies, 30 assertions)

```bash
cmake --build build --target pqchsm-p11
"$(brew --prefix openjdk)/libexec/openjdk.jdk/Contents/Home/bin/java" \
  --enable-native-access=ALL-UNNAMED \
  demo/java/PqcHsmDemo.java "$PWD/build/pqchsm-pkcs11.dylib"
```

## Compatibility with higher-level frameworks

Both demos deliberately use the **low-level** PKCS#11 binding and pass mechanism codes
directly. The high-level provider frameworks in both ecosystems do not yet support
ML-KEM/ML-DSA over PKCS#11. The findings below are measured, not assumed.

### Python

| Route | ML-KEM / ML-DSA | Notes |
|---|---|---|
| **PyKCS11** (SWIG wrapper over the C API) | **works** | used by this demo |
| python-pkcs11 (high-level) | does not work | mechanisms and attributes are its own enums, with no ML-KEM/ML-DSA entries |

PyKCS11 works but is not turnkey. Three adaptation gaps were encountered; each is
documented inline in `demo/python/pqchsm_demo.py`:

1. `lib.getMechanismList(slot)` raises `KeyError: 28`. It reverse-looks-up mechanism
   code `0x1C` in its bundled `CKM` dictionary and fails when the entry is absent.
   *Workaround:* `PyKCS11.CKM` / `CKA` / `CKK` are ordinary Python dicts — register the
   PQC constants into them (`register_pqc_constants()` in the demo).
2. Passing `CKA_PARAMETER_SET` raises
   `TypeError: argument 3 of type 'std::vector<unsigned char> const &'`. PyKCS11 uses an
   internal type table to choose between `SetNum`/`SetBool`/`SetBin`; unknown attributes
   default to `SetBin`. *Workaround:* encode the `CK_ULONG` as little-endian bytes.
3. Reading numeric attributes hits the same table. *Workaround:* request
   `allAsBinary=True` and decode the raw bytes.

In short: PyKCS11 lacks constant and type tables, not capability.

### Java

| Route | ML-KEM / ML-DSA | Notes |
|---|---|---|
| **JDK FFM calling the C ABI** | **works** | used by this demo, zero external dependencies |
| SunPKCS11 (high-level JCA) | does not work | see below |
| IAIK PKCS#11 Wrapper, jacknji11 | would work | both require additional jars; FFM was sufficient |

`demo/java/SunP11Probe.java` measures the exact limitation on JDK 26:

```
provider = SunPKCS11-pqchsm  version 26
services exposed = 1
   KeyStore / PKCS11
  ML-DSA     via P11 : unavailable -> NoSuchAlgorithmException
  ML-DSA-65  via P11 : unavailable -> NoSuchAlgorithmException
  ML-KEM     via P11 : unavailable -> NoSuchAlgorithmException
  ML-KEM-768 via P11 : unavailable -> NoSuchAlgorithmException
```

SunPKCS11 loads this module successfully — the provider constructs and works as a
`KeyStore` — but its mapping from PKCS#11 mechanism codes to JCA algorithm names does
not yet include v3.2's `CKM_ML_DSA` (0x1D) or `CKM_ML_KEM` (0x17), so no
`KeyPairGenerator` or `Signature` service is registered.

Two distinct things are worth separating:

- JDK 26's JCA **does** know ML-DSA and ML-KEM — `KeyPairGenerator.getInstance("ML-DSA")`
  works with the software implementation (JEP 496/497).
- What is missing is the path *through a PKCS#11 provider* to an external module.

This is not specific to this module: any PKCS#11 module offering ML-DSA would be
equally invisible to SunPKCS11. It requires JDK to add the mechanisms to
`sun.security.pkcs11.wrapper.PKCS11Constants` and to the `SunPKCS11` service registry.

FFM was chosen over IAIK/jacknji11 because it ships with JDK 22+ and needs no jar, and
because this module exports every `C_*` entry point as a global symbol — so
`SymbolLookup.find()` by name is enough, with no need to walk `CK_FUNCTION_LIST` struct
offsets.

## Environment variables

Both demos configure the module through environment variables (see
`src/p11/p11_module.c`):

| Variable | Default | Meaning |
|---|---|---|
| `PQCHSM_KEYSTORE` | `$HOME/.pqchsm/keystore.bin` | keystore path |
| `PQCHSM_SLOTS` | 4 | number of slots |

The demos point at a temporary keystore and do not touch the default one.
