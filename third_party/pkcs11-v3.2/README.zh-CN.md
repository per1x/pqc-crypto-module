[English](README.md) · **中文**

# OASIS PKCS#11 v3.2 头文件（vendored）

来源：<https://docs.oasis-open.org/pkcs11/pkcs11-spec/v3.2/os/include/pkcs11-v3.2/>
取得于 2026-07-29，**未作任何修改**。

之所以 vendored 而不是取自系统软件包，有两个原因。PKCS#11 是 ABI 契约，头文件版本
必须精确可控。而且 v3.2 是首个原生定义 ML-KEM 与 ML-DSA 的修订版——`CKM_ML_KEM`、
`CKM_ML_DSA`、`CKK_ML_KEM`、`CKK_ML_DSA`、各 `CKP_*` 参数集，以及
`C_EncapsulateKey` / `C_DecapsulateKey` 函数。各发行版通常仍通过 OpenSC 提供
v3.0 或 v3.1，其中不含上述任何一项。

这些头文件刻意不包含平台宏（`CK_PTR`、`CK_DECLARE_FUNCTION` 等）；它们由
`src/p11/p11_config.h` 提供。

这三个文件可按 OASIS 条款自由复制使用。
