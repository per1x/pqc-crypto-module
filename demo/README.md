# demo/ —— 把 pqc-hsm 当作 PKCS#11 provider 在 Python / Java 里调用

两个 demo 都走**完整流程**：加载模块 → C_Initialize → 初始化 token 与 PIN →
登录 → 生成 ML-DSA / ML-KEM 密钥到槽位 → 签名/验签 → 读属性 → 查找对象。

## 可运行命令

**Python**（PyKCS11，25 项断言）

```bash
cd /Users/jinling/code/pqc-hsm && python3 -m venv .venv-p11 && ./.venv-p11/bin/pip install -q PyKCS11 && cmake --build build --target pqchsm-p11 && ./.venv-p11/bin/python demo/python/pqchsm_demo.py
```

**Java**（JDK 22+ 自带的 FFM，零外部依赖，30 项断言）

```bash
cd /Users/jinling/code/pqc-hsm && cmake --build build --target pqchsm-p11 && "$(brew --prefix openjdk)/libexec/openjdk.jdk/Contents/Home/bin/java" --enable-native-access=ALL-UNNAMED demo/java/PqcHsmDemo.java "$PWD/build/pqchsm-pkcs11.dylib"
```

---

## 兼容性结论（实测，不是推测）

### Python

| 路线 | 能否跑通 ML-KEM/ML-DSA | 说明 |
|------|----------------------|------|
| **PyKCS11**（SWIG 直接封装 C API） | **能** ✅ | 本 demo 用的就是它 |
| python-pkcs11（高层封装） | **不能** ❌ | 机制/属性是它自己的枚举，没有 ML-KEM/ML-DSA 条目 |

PyKCS11 虽然能用，但**不是开箱即用**，实测踩到三处适配缺口，都写在
`demo/python/pqchsm_demo.py` 的注释里：

1. `lib.getMechanismList(slot)` 直接抛 `KeyError: 28` ——
   它拿到机制码 0x1C 后去自带的 `CKM` 字典反查名字，查不到就炸。
   **解法**：`PyKCS11.CKM` / `CKA` / `CKK` 都是普通 Python dict，
   把 PQC 常量注册进去即可（demo 里的 `register_pqc_constants()`）。
2. 传 `CKA_PARAMETER_SET` 时报
   `TypeError: argument 3 of type 'std::vector<unsigned char> const &'` ——
   PyKCS11 靠内部类型表决定属性用 SetNum/SetBool/SetBin 写入，
   新属性不在表里就默认走 SetBin。**解法**：自己按 CK_ULONG 小端编成字节再传。
3. 读数值属性时同理。**解法**：`allAsBinary=True` 取原始字节自己解。

结论：**PyKCS11 缺的只是常量表和类型表，不是能力** —— 补上就能按机制码直调。

### Java

| 路线 | 能否跑通 ML-KEM/ML-DSA | 说明 |
|------|----------------------|------|
| **JDK FFM 直调 C ABI** | **能** ✅ | 本 demo 用的就是它，**零外部依赖** |
| SunPKCS11（JCA 高层） | **不能** ❌ | 见下 |
| IAIK PKCS#11 Wrapper / jacknji11 | 能（未采用） | 都要额外 jar；FFM 已经够用就没引 |

**SunPKCS11 的确切限制**（JDK 26 实测，`demo/java/SunP11Probe.java`）：

```
provider = SunPKCS11-pqchsm  版本 26
暴露的服务数 = 1
   KeyStore / PKCS11
  经 P11 的 ML-DSA    : 不可用 -> NoSuchAlgorithmException
  经 P11 的 ML-DSA-65 : 不可用 -> NoSuchAlgorithmException
  经 P11 的 ML-KEM    : 不可用 -> NoSuchAlgorithmException
  经 P11 的 ML-KEM-768: 不可用 -> NoSuchAlgorithmException
```

也就是说：**SunPKCS11 能成功加载本模块**（provider 建得起来、能当 KeyStore 用），
**但它的「PKCS#11 机制码 → JCA 算法名」映射表还没有加入 v3.2 的
`CKM_ML_DSA`(0x1d) / `CKM_ML_KEM`(0x17)**，所以拿不到对应的
`KeyPairGenerator` / `Signature` 服务。

请注意区分两件事：
- JDK 26 的 JCA **本身**是认识 ML-DSA/ML-KEM 的
  （`KeyPairGenerator.getInstance("ML-DSA")` 用软件实现可用，JEP 496/497）；
- 缺的是**"经由 PKCS#11 provider 去用硬件/外部模块"**这条路。

这不是本模块的问题 —— 换任何一个支持 ML-DSA 的 PKCS#11 模块，
SunPKCS11 都一样认不出来。要走通得等 JDK 把新机制加进
`sun.security.pkcs11.wrapper.PKCS11Constants` 与 `SunPKCS11` 的服务注册表。

**为什么选 FFM 而不是 IAIK/jacknji11**：都能直传机制码，但 FFM 是 JDK 22+ 自带的，
不需要下载任何 jar；而且本模块把 27 个 `C_*` 都导出成了全局符号，
连 `CK_FUNCTION_LIST` 的结构偏移都不用数，直接按名字 `SymbolLookup.find()`。

---

## 环境变量

两个 demo 都通过环境变量控制模块行为（见 `src/p11/p11_module.c`）：

| 变量 | 默认 | 说明 |
|------|------|------|
| `PQCHSM_KEYSTORE` | `$HOME/.pqchsm/keystore.bin` | 密钥库路径 |
| `PQCHSM_SLOTS` | 4 | 槽位数 |

demo 会自己指向一个临时密钥库，不会污染你的默认库。
