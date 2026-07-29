**English** · [中文](README.zh-CN.md)

# OASIS PKCS#11 v3.2 headers (vendored)

Source: <https://docs.oasis-open.org/pkcs11/pkcs11-spec/v3.2/os/include/pkcs11-v3.2/>
Retrieved 2026-07-29, **unmodified**.

These are vendored rather than taken from a system package for two reasons. PKCS#11 is
an ABI contract, so the header version must be exactly controlled. And v3.2 is the first
revision to define ML-KEM and ML-DSA natively — `CKM_ML_KEM`, `CKM_ML_DSA`,
`CKK_ML_KEM`, `CKK_ML_DSA`, the `CKP_*` parameter sets, and the `C_EncapsulateKey` /
`C_DecapsulateKey` functions. Distributions typically still ship v3.0 or v3.1 via
OpenSC, which lacks all of them.

The headers deliberately omit platform macros (`CK_PTR`, `CK_DECLARE_FUNCTION`, and so
on); those are supplied by `src/p11/p11_config.h`.

These three files may be freely copied and used under the OASIS terms.
