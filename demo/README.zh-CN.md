[English](README.md) · **中文**

# 演示

| 目录 | 内容 |
|---|---|
| [functions/](functions/) | 主机侧密码模块的管理功能一览：算法支持性、熵源、槽位、密钥、M-of-N 备份、安全存储。不需要板子 |
| [remote/](remote/) | 经 mTLS 对板子运行 SDF 接口演示：`./demo/remote/run.sh` |
| `python/`、`java/` | PKCS#11 provider 演示（见下）。默认走软件后端，不需要板子 |

板上演示见 [`../board/demo/README.zh-CN.md`](../board/demo/README.zh-CN.md)。

## PKCS#11 provider 演示

两个演示把本模块作为 PKCS#11 provider 加载，运行完整的密钥生命周期：`C_Initialize`、初始化令牌和 PIN、登录、生成 ML-DSA 与 ML-KEM 密钥、签名验签、读取属性、枚举对象。以下命令都在仓库根目录执行。

### Python（PyKCS11，25 项断言）

```bash
cmake --build build --target pqchsm-p11
python3 -m venv .venv-p11 && ./.venv-p11/bin/pip install -q PyKCS11
./.venv-p11/bin/python demo/python/pqchsm_demo.py
```

脚本自己建临时密钥库（`PQCHSM_SLOTS=3`）。模块路径可以作为第一个参数传入。

### Java（JDK 22+ FFM，无外部依赖，30 项断言）

```bash
cmake --build build --target pqchsm-p11
PQCHSM_KEYSTORE=$(mktemp -d)/keystore.bin \
  "$(brew --prefix openjdk)/libexec/openjdk.jdk/Contents/Home/bin/java" \
  --enable-native-access=ALL-UNNAMED \
  demo/java/PqcHsmDemo.java
```

演示从 `C_InitToken` 开始，需要一个尚未初始化的密钥库，所以像上面这样用新的 `PQCHSM_KEYSTORE`。对已有的密钥库运行会报 18 项通过、12 项失败。模块路径可以作为第一个参数传入，不传则依次尝试 `build/pqchsm-pkcs11.so` 和 `.dylib`。系统没有 UTF-8 locale 时加 `LANG=C.UTF-8`，否则中文输出显示为 `?`。

`tools/demo_p11.sh` 一次跑两个演示，缺环境时跳过。

## 为什么用低层绑定

两个演示都用低层 PKCS#11 绑定，直接传机制码，因为高层框架还不支持经 PKCS#11 使用 ML-KEM 和 ML-DSA。

### Python

| 路线 | ML-KEM / ML-DSA | 说明 |
|---|---|---|
| PyKCS11（SWIG 封装 C API） | 可用 | 本演示使用 |
| python-pkcs11（高层） | 不可用 | 机制与属性是它自己的枚举，没有 ML-KEM/ML-DSA 条目 |

PyKCS11 需要三处处理，都在 `demo/python/pqchsm_demo.py` 里：

1. `lib.getMechanismList(slot)` 抛 `KeyError: 28`：它在 `CKM` 字典里反查机制码 `0x1C`。`PyKCS11.CKM`、`CKA`、`CKK` 是普通 dict，演示把 PQC 常量注册进去（`register_pqc_constants()`）。
2. 传 `CKA_PARAMETER_SET` 抛 `TypeError`：PyKCS11 按内部类型表选 `SetNum`/`SetBool`/`SetBin`，不认识的属性按二进制处理。演示把 `CK_ULONG` 编成小端字节。
3. 读数值属性遇到同一张表。演示用 `allAsBinary=True` 取原始字节再解码。

### Java

| 路线 | ML-KEM / ML-DSA | 说明 |
|---|---|---|
| JDK FFM 调 C ABI | 可用 | 本演示使用，不需要 jar |
| SunPKCS11（JCA） | 不可用 | 见下 |
| IAIK PKCS#11 Wrapper、jacknji11 | 未试 | 都需要额外的 jar |

`demo/java/SunP11Probe.java` 展示 SunPKCS11 的限制（JDK 26 实测）。provider 能加载本模块，并作为 `KeyStore` 工作，但只暴露这一个服务。它把 PKCS#11 机制码映射到 JCA 算法名的表里没有 PKCS#11 v3.2 的 `CKM_ML_DSA`（0x1D）和 `CKM_ML_KEM`（0x17），所以 `KeyPairGenerator.getInstance("ML-DSA", p11Provider)` 以及 ML-KEM 的同类调用都抛 `NoSuchAlgorithmException`。JDK 26 自己的 JCA 支持 ML-DSA 和 ML-KEM 的软件实现，缺的只是经 PKCS#11 provider 的这条路。任何提供 ML-DSA 的 PKCS#11 模块都会遇到同样的问题。

选 FFM 是因为 JDK 22+ 自带，并且本模块把每个 `C_*` 函数都导出为全局符号，按名字 `SymbolLookup.find()` 就够了，不需要解析 `CK_FUNCTION_LIST` 的偏移。

## 环境变量

模块读取以下变量（`src/p11/p11_module.c`）：

| 变量 | 默认 | 含义 |
|---|---|---|
| `PQCHSM_KEYSTORE` | `$HOME/.pqchsm/keystore.bin` | 密钥库路径 |
| `PQCHSM_SLOTS` | 4 | 槽位数（1 到 64） |
| `PQCHSM_BACKEND` | 软件（liboqs） | `sdfe`：ML-KEM 密钥生成经 `libsdfe` 交给板子 |
| `PQCHSM_HWRNG` | 软件 | `sdfe`：随机数取自板上 TRNG |
| `PQCHSM_SDFE_HOST`、`_PORT`、`_PKI`、`_DEVICE_CN` | | `sdfe` 选项用的板子地址、端口、凭据目录、期望的设备 CN |

两个演示都用临时密钥库，不碰默认的那份。
