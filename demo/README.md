**English** · [中文](README.zh-CN.md)

# Demos

| Directory | Contents |
|---|---|
| [functions/](functions/) | Management-function tour of the host-side crypto module: algorithm support, entropy, slots, keys, M-of-N backup, secure storage. No board needed |
| [remote/](remote/) | Runs the SDF-interface demo against the board over mTLS: `./demo/remote/run.sh` |
| `python/`, `java/` | PKCS#11 provider demos (below). Software backend by default, no board needed |

The on-board demos are described in [`../board/demo/README.zh-CN.md`](../board/demo/README.zh-CN.md).

## PKCS#11 provider demos

The two demos load the module as a PKCS#11 provider and run a full key lifecycle: `C_Initialize`, initialise the token and PINs, log in, generate ML-DSA and ML-KEM keys, sign and verify, read attributes, enumerate objects. All commands run from the repository root.

### Python (PyKCS11, 25 assertions)

```bash
cmake --build build --target pqchsm-p11
python3 -m venv .venv-p11 && ./.venv-p11/bin/pip install -q PyKCS11
./.venv-p11/bin/python demo/python/pqchsm_demo.py
```

The script creates its own temporary keystore (`PQCHSM_SLOTS=3`). The module path may be given as the first argument.

### Java (JDK 22+ FFM, no external dependencies, 30 assertions)

```bash
cmake --build build --target pqchsm-p11
PQCHSM_KEYSTORE=$(mktemp -d)/keystore.bin \
  "$(brew --prefix openjdk)/libexec/openjdk.jdk/Contents/Home/bin/java" \
  --enable-native-access=ALL-UNNAMED \
  demo/java/PqcHsmDemo.java
```

The demo starts with `C_InitToken`, so it needs a keystore that is not yet initialised; use a fresh `PQCHSM_KEYSTORE` as above. Against an existing keystore it reports 18 passed and 12 failed. The module path may be given as the first argument; otherwise `build/pqchsm-pkcs11.so` and `.dylib` are tried. Without a UTF-8 locale, set `LANG=C.UTF-8`, or the Chinese output prints as `?`.

`tools/demo_p11.sh` runs both demos and skips them if the environment is missing.

## Why the low-level bindings

Both demos use a low-level PKCS#11 binding and pass the mechanism codes directly, because the high-level frameworks do not yet support ML-KEM and ML-DSA over PKCS#11.

### Python

| Route | ML-KEM / ML-DSA | Notes |
|---|---|---|
| PyKCS11 (SWIG wrapper over the C API) | works | used here |
| python-pkcs11 (high-level) | does not work | mechanisms and attributes are its own enums, with no ML-KEM/ML-DSA entries |

PyKCS11 needs three workarounds, all in `demo/python/pqchsm_demo.py`:

1. `lib.getMechanismList(slot)` raises `KeyError: 28`: it looks up mechanism code `0x1C` in its `CKM` dictionary. `PyKCS11.CKM`, `CKA` and `CKK` are plain dicts, so the demo registers the PQC constants into them (`register_pqc_constants()`).
2. Passing `CKA_PARAMETER_SET` raises `TypeError`: PyKCS11 picks `SetNum`/`SetBool`/`SetBin` from an internal type table and treats unknown attributes as binary. The demo encodes the `CK_ULONG` as little-endian bytes.
3. Reading numeric attributes hits the same table. The demo requests `allAsBinary=True` and decodes the bytes.

### Java

| Route | ML-KEM / ML-DSA | Notes |
|---|---|---|
| JDK FFM calling the C ABI | works | used here, no jar needed |
| SunPKCS11 (JCA) | does not work | see below |
| IAIK PKCS#11 Wrapper, jacknji11 | not tried | both need extra jars |

`demo/java/SunP11Probe.java` shows the SunPKCS11 limitation (measured on JDK 26). The provider loads this module and works as a `KeyStore`, but it exposes only that one service. Its table from PKCS#11 mechanism codes to JCA algorithm names has no entry for the PKCS#11 v3.2 mechanisms `CKM_ML_DSA` (0x1D) and `CKM_ML_KEM` (0x17), so `KeyPairGenerator.getInstance("ML-DSA", p11Provider)` and the ML-KEM equivalents throw `NoSuchAlgorithmException`. JDK 26's own JCA does support ML-DSA and ML-KEM with the software implementation; only the path through a PKCS#11 provider is missing. Any PKCS#11 module offering ML-DSA is affected in the same way.

FFM is used because it ships with JDK 22+ and the module exports every `C_*` function as a global symbol, so `SymbolLookup.find()` by name is enough and the `CK_FUNCTION_LIST` offsets are not needed.

## Environment variables

Read by the module (`src/p11/p11_module.c`):

| Variable | Default | Meaning |
|---|---|---|
| `PQCHSM_KEYSTORE` | `$HOME/.pqchsm/keystore.bin` | Keystore path |
| `PQCHSM_SLOTS` | 4 | Number of slots (1 to 64) |
| `PQCHSM_BACKEND` | software (liboqs) | `sdfe`: ML-KEM key generation goes to the board through `libsdfe` |
| `PQCHSM_HWRNG` | software | `sdfe`: random numbers come from the board's TRNG |
| `PQCHSM_SDFE_HOST`, `_PORT`, `_PKI`, `_DEVICE_CN` | | Board address, port, credential directory and expected device CN for the `sdfe` options |

Both demos use a temporary keystore and do not touch the default one.
