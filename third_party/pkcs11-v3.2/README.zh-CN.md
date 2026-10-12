[English](README.md) · **中文**

# OASIS PKCS#11 v3.2 头文件（vendored）

`pkcs11.h`、`pkcs11f.h`、`pkcs11t.h` 取自
<https://docs.oasis-open.org/pkcs11/pkcs11-spec/v3.2/os/include/pkcs11-v3.2/>，
2026-07-29 取得，未作修改。

放进仓库是因为 PKCS#11 是 ABI，头文件版本必须固定；另外 v3.2 是第一个定义 ML-KEM 和
ML-DSA 的版本：`CKM_ML_KEM`、`CKM_ML_DSA`、`CKK_ML_KEM`、`CKK_ML_DSA`、各 `CKP_*`
参数集，以及 `C_EncapsulateKey` / `C_DecapsulateKey`。发行版通常只带 v3.0 或 v3.1
（例如随 OpenSC 提供），这些都没有。

头文件里没有定义平台宏（`CK_PTR`、`CK_DECLARE_FUNCTION` 等），这些由
`src/p11/p11_config.h` 定义。

文件按随附的 OASIS 条款使用。
