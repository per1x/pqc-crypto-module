**English** · [中文](README.zh-CN.md)

# OASIS PKCS#11 v3.2 headers (vendored)

`pkcs11.h`, `pkcs11f.h` and `pkcs11t.h` from
<https://docs.oasis-open.org/pkcs11/pkcs11-spec/v3.2/os/include/pkcs11-v3.2/>,
retrieved 2026-07-29 and not modified.

They are vendored because PKCS#11 is an ABI, so the header version has to be
fixed, and because v3.2 is the first revision that defines ML-KEM and ML-DSA:
`CKM_ML_KEM`, `CKM_ML_DSA`, `CKK_ML_KEM`, `CKK_ML_DSA`, the `CKP_*` parameter
sets, and `C_EncapsulateKey` / `C_DecapsulateKey`. Distributions usually ship
v3.0 or v3.1 (for example with OpenSC), which has none of these.

The headers do not define the platform macros (`CK_PTR`, `CK_DECLARE_FUNCTION`
and so on); `src/p11/p11_config.h` defines them.

The files are used under the OASIS terms that come with them.
