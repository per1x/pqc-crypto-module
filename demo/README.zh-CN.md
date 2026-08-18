[English](README.md) · **中文**

# 演示

| | |
|---|---|
| [functions/](functions/) | **密码机功能一览**：算法支持性、熵源、槽位、密钥、M-of-N 备份、安全存储。不需要板子。 |
| [remote/](remote/) | **一条命令直接打到真板子**：`./demo/remote/run.sh` —— 地址它会问你，不用 SSH、不用装凭据。先看这个。 |
| 下面的 Python / Java | PKCS#11 provider 的完整密钥生命周期。默认走软件后端，不需要板子。 |

## PKCS#11 provider 演示

两个演示程序把本模块作为 PKCS#11 provider 加载，并驱动完整的密钥生命周期：
`C_Initialize` → 初始化令牌与 PIN → 登录 → 在槽位中生成 ML-DSA 与 ML-KEM 密钥 →
签名与验签 → 读取属性 → 枚举对象。

以下命令均假定工作目录为仓库根。

## Python（PyKCS11，25 项断言）

```bash
cmake --build build --target pqchsm-p11
python3 -m venv .venv-p11 && ./.venv-p11/bin/pip install -q PyKCS11
./.venv-p11/bin/python demo/python/pqchsm_demo.py
```

## Java（JDK 22+ FFM，无外部依赖，30 项断言）

```bash
cmake --build build --target pqchsm-p11
PQCHSM_KEYSTORE=$(mktemp -d)/keystore.bin \
  "$(brew --prefix openjdk)/libexec/openjdk.jdk/Contents/Home/bin/java" \
  --enable-native-access=ALL-UNNAMED \
  demo/java/PqcHsmDemo.java
```

这个演示从 `C_InitToken` 开始，需要一个尚未初始化的密钥库，因此每次给一个新的
`PQCHSM_KEYSTORE`。对着已经初始化过的密钥库跑会得到"通过 18，失败 12"。
Python 演示自己建临时密钥库，没有这个问题。模块路径可以作为第一个参数传入；
不传则在 `build/` 下按平台探测 `.so` 与 `.dylib`。系统没有 UTF-8 locale 时
要加 `LANG=C.UTF-8`，否则中文输出显示成 `?`。

## 与高层框架的兼容性

两个演示都刻意使用**低层** PKCS#11 绑定并直接传递机制码。两个生态的高层 provider
框架尚不支持经 PKCS#11 使用 ML-KEM/ML-DSA。以下结论均为实测，非推断。

### Python

| 路线 | ML-KEM / ML-DSA | 说明 |
|---|---|---|
| **PyKCS11**（对 C API 的 SWIG 封装） | **可用** | 本演示所采用 |
| python-pkcs11（高层封装） | 不可用 | 机制与属性是它自己的枚举，没有 ML-KEM/ML-DSA 条目 |

PyKCS11 可用但并非开箱即用，存在三处适配缺口，均在 `demo/python/pqchsm_demo.py`
中就地处理：

1. `lib.getMechanismList(slot)` 抛出 `KeyError: 28`。它拿到机制码 `0x1C` 后到自带的
   `CKM` 字典反查名字，条目不存在时失败。
   *处理方式：*`PyKCS11.CKM` / `CKA` / `CKK` 都是普通 Python dict，把 PQC 常量注册
   进去即可（演示中的 `register_pqc_constants()`）。
2. 传入 `CKA_PARAMETER_SET` 时抛出
   `TypeError: argument 3 of type 'std::vector<unsigned char> const &'`。PyKCS11 依据
   内部类型表在 `SetNum`/`SetBool`/`SetBin` 之间选择，未知属性默认走 `SetBin`。
   *处理方式：*把 `CK_ULONG` 按小端编码成字节再传。
3. 读取数值属性时命中同一张表。*处理方式：*请求 `allAsBinary=True` 并自行解码原始
   字节。

简而言之：PyKCS11 缺的是常量表与类型表，不是能力。

### Java

| 路线 | ML-KEM / ML-DSA | 说明 |
|---|---|---|
| **JDK FFM 直调 C ABI** | **可用** | 本演示所采用，零外部依赖 |
| SunPKCS11（高层 JCA） | 不可用 | 见下 |
| IAIK PKCS#11 Wrapper、jacknji11 | 可用 | 均需额外 jar；FFM 已足够 |

`demo/java/SunP11Probe.java` 在 JDK 26 上测得的确切限制：

```
provider = SunPKCS11-pqchsm  version 26
services exposed = 1
   KeyStore / PKCS11
  ML-DSA     via P11 : unavailable -> NoSuchAlgorithmException
  ML-DSA-65  via P11 : unavailable -> NoSuchAlgorithmException
  ML-KEM     via P11 : unavailable -> NoSuchAlgorithmException
  ML-KEM-768 via P11 : unavailable -> NoSuchAlgorithmException
```

SunPKCS11 能成功加载本模块——provider 建得起来，也能当作 `KeyStore` 使用——但它从
PKCS#11 机制码到 JCA 算法名的映射表尚未包含 v3.2 的 `CKM_ML_DSA`（0x1D）与
`CKM_ML_KEM`（0x17），因此不会注册任何 `KeyPairGenerator` 或 `Signature` 服务。

有两件事值得区分：

- JDK 26 的 JCA **本身**是认识 ML-DSA 与 ML-KEM 的——
  `KeyPairGenerator.getInstance("ML-DSA")` 用软件实现可用（JEP 496/497）。
- 缺失的是**经由 PKCS#11 provider 通向外部模块**的这条路径。

这不是本模块特有的问题：任何提供 ML-DSA 的 PKCS#11 模块对 SunPKCS11 同样不可见。
需要 JDK 把这些机制加入 `sun.security.pkcs11.wrapper.PKCS11Constants` 与
`SunPKCS11` 的服务注册表。

选择 FFM 而非 IAIK/jacknji11，是因为它随 JDK 22+ 自带、不需要任何 jar；而且本模块把
每个 `C_*` 入口都导出为全局符号，按名字 `SymbolLookup.find()` 即可，无需推算
`CK_FUNCTION_LIST` 的结构偏移。

## 环境变量

两个演示都通过环境变量配置模块（见 `src/p11/p11_module.c`）：

| 变量 | 默认值 | 含义 |
|---|---|---|
| `PQCHSM_KEYSTORE` | `$HOME/.pqchsm/keystore.bin` | 密钥库路径 |
| `PQCHSM_SLOTS` | 4 | 槽位数 |

演示程序指向临时密钥库，不会影响默认密钥库。
