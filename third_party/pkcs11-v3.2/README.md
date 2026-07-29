# PKCS#11 v3.2 官方头文件（vendored）

来源：OASIS PKCS#11 v3.2 规范配套头文件
`https://docs.oasis-open.org/pkcs11/pkcs11-spec/v3.2/os/include/pkcs11-v3.2/`
（取得日期 2026-07-29，**未做任何修改**）

之所以 vendored 而不是依赖系统包：PKCS#11 是 ABI 契约，头文件版本必须精确可控；
而且 v3.2 才**原生定义** ML-KEM / ML-DSA 的机制码与参数集常量
（`CKM_ML_KEM`、`CKM_ML_DSA`、`CKK_ML_KEM`、`CKK_ML_DSA`、`CKP_ML_DSA_65` 等），
以及 `C_EncapsulateKey` / `C_DecapsulateKey` —— 系统上装的 OpenSC 通常还是 v3.0/v3.1。

这三个文件按 OASIS 的条款可自由复制使用。
