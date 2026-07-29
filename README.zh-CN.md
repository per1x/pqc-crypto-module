[English](README.md) · **中文**

# pqc-crypto-module

后量子密码模块原型：密钥存储、槽位管理、备份与恢复、防篡改审计日志、PKCS#11 v3.2
前端，以及将算术核迁入 FPGA 所需的 RTL 与仿真环境。

> **状态：研究原型。** 安全边界是进程地址空间，不是硬件。请勿用于保护真实资产。
> 参见[安全模型与局限](#安全模型与局限)。

## 概述

后量子算法已经标准化（FIPS 203 / 204），但真正决定一个密码模块成败的是它周边的
机制——密钥如何存储、包裹、备份、吊销，以及如何暴露给应用。本项目围绕 ML-KEM 与
ML-DSA 构建这套机制，并按真实设备的形态组织，使这部分软件今后可以整体迁移到
Zynq 级 SoC 上，由可编程逻辑承载算法核。

这个目标带来两条贯穿全局的设计约束：

- **一切经由句柄。** `include/pqchsm/` 中没有任何 API 返回私钥材料。密钥库、槽位
  管理器与 PKCS#11 前端全部基于不透明句柄工作，因此今后收紧安全边界不会影响任何
  调用方。
- **硬件接缝从第一天起就存在。** 密码运算经过一层 vtable（`pqc_backend_t`），其下
  是 AXI 风格的寄存器接口（`accel_transport_t`）。三种 transport 以完全相同的方式
  实现该接口：软件桩、Verilator 仿真的 RTL 后端，以及在有硬件之后的 `/dev/mem` +
  `mmap`。在它们之间切换不会改变接缝之上的任何代码。

运行路径上的每一次密码运算都由 [liboqs](https://github.com/open-quantum-safe/liboqs)
或 OpenSSL 完成，没有自行实现的生产用原语。仓库中确实存在手写的 NTT 与 Keccak 实现
（`src/hal/accel_stub.c`、`hardware/model/`），但它们仅作为 RTL 对拍的参照物，绝不
用于保护密钥材料。正确性以 NIST ACVP 向量为准。

## 架构

```
                PKCS#11 v3.2 共享库          (src/p11)
                守护进程 / CLI / 管理工具    (cli)
 ──────────────────────────────────────────────────────────────────
  槽位管理器      密钥库          备份 / 恢复          审计链
  (src/slot)      (src/store)     (src/backup)         (src/audit)
    状态机、      AES-256-GCM     GF(256) 上的         SHA3-256 哈希链
    会话、        包裹、          Shamir M-of-N，      + ML-DSA 锚点
    句柄、        原子写          设备绑定 KEK           签名
    访问控制                      与可迁移 BEK
 ──────────────────────────────────────────────────────────────────
                  pqc_backend_t vtable  (include/pqchsm/pqc.h)
                             │
             ┌───────────────┴────────────────┐
       liboqs 后端                      寄存器接口后端
       (src/crypto)                    (src/hal/pqc_accel.c)
                                               │
                             accel_transport_t (include/pqchsm/accel.h)
                                               │
                       ┌───────────────────────┼───────────────────┐
                    软件桩                 Verilator RTL          mmap
                  (accel_stub.c)      (accel_verilator.c)        (未来)
                                               │
                                     hardware/rtl: ntt_core,
                                                   keccak_f1600
```

### 密钥层级

设备绑定的根密钥（`KDR`，当前为桩——见局限）派生出 **KEK**，用于包裹静态密钥。备份
则使用 **BEK**，它由随机生成的恢复主密钥派生，并以 GF(256) 上的 Shamir 方案拆分，
乘法为常数时间实现。两者分开是有意为之：KEK 包裹的材料不能离开设备，BEK 包裹的材料
可迁移到替换设备，而某个槽位是否参与备份需要显式选择。

### 审计链

每次状态转换都追加一条记录，其哈希覆盖前一条记录的哈希。纯哈希链只能保证篡改会向后
传播，因此链头还会用 ML-DSA 设备身份密钥签名并锚定到设备之外——否则重写整个文件是
无法察觉的。

### 硬件接缝

`accel.h` 在编写任何 RTL 之前就固定了寄存器表（`CTRL`、`STATUS`、`MODE`、`PARAM`、
`IN_LEN`、`OUT_LEN`、`ERRCODE`），使接口层面的错误在软件阶段就暴露。目前有两个核以
RTL 实现，并通过同一套寄存器接口在 Verilator 下运行：

| 核 | 结构 | 周期数 |
|---|---|---|
| `ntt_core` | 单蝶形单元，ML-KEM 的 7 层 NTT | 1153 / 次变换 |
| `keccak_f1600` | 单轮迭代，`round_cnt` 走 24 轮 | 24 / 次置换 |

SHA3 与 SHAKE 的海绵结构在 C 侧实现、置换交给硬件，因此整条 SHA3/SHAKE 路径都能跑
在仿真 RTL 上，并与 OpenSSL 逐字节比对。其余加速模式回退到软件桩，且 Verilator
transport 会明确报"不支持"，而不是悄悄用软件顶替。

## 特性

- **ML-KEM-512/768/1024 与 ML-DSA-44/65/87**，以 390 条 NIST ACVP 向量逐字节验证
  （向量固定到特定的 ACVP-Server commit）。
- **槽位/令牌/对象/会话模型**，含显式状态转换表、基于角色的访问控制、PIN 锁定，以及
  使旧句柄立即失效的世代计数器。
- **加密密钥库**——AES-256-GCM 包裹、元数据作为 AAD、整文件 KMAC，以及
  `tmp → fsync → rename → fsync(dir)` 的原子写。
- **跨设备的 M-of-N 备份与恢复**，并以每槽位策略控制某把密钥是否允许进入备份。
- **防篡改审计日志**，含 ML-DSA 锚点签名。
- **安全密钥注入**——用设备自身的 ML-KEM 公钥封装一次性会话密钥，明文密钥材料不出现
  在链路上。
- **PKCS#11 v3.2 前端**，暴露原生的 `CKM_ML_KEM` / `CKM_ML_DSA` 机制，其中
  `C_EncapsulateKey` / `C_DecapsulateKey` 经 `C_GetInterface` 获取。
- **带独立验证的 RTL 核**——cocotb 测试台对照公开的 Keccak 全零置换向量以及
  `hashlib`/OpenSSL，而不仅仅是本项目自己的参考模型。

## 仓库结构

```
├── include/pqchsm/     公共头文件——全部 API 面。私钥不跨越它。
├── src/
│   ├── crypto/         liboqs 绑定、KDF、密钥派生根（KDR）
│   ├── slot/           槽位状态机、会话、元数据、持久化
│   ├── store/          AES-256-GCM 包裹、密钥库文件格式
│   ├── backup/         Shamir 拆分、备份与恢复、密钥注入
│   ├── audit/          哈希链与 ML-DSA 锚定
│   ├── hal/            加速器抽象：软件桩、Verilator、寄存器语义
│   ├── p11/            PKCS#11 v3.2 共享库
│   ├── proto/          TLV 命令协议
│   └── util/           安全清零、锁页分配、KAT 解析
├── cli/                守护进程、客户端 CLI、管理工具
├── tests/              单元、集成、KAT 与 fuzz 靶子
├── demo/               PKCS#11 provider 演示（Python、Java）
├── hardware/
│   ├── rtl/            Verilog 源码：mlkem/（NTT）、keccak/
│   ├── tb/cocotb/      cocotb 测试台
│   ├── model/          Python 参考模型、向量导出、独立预言机
│   └── syn/            Vivado out-of-context 综合脚本与约束
├── tools/              向量获取、基准测试、剖析、回归脚本
├── third_party/        vendored 的 OASIS PKCS#11 v3.2 头文件（未作修改）
└── docs/               架构与 PKCS#11 接口参考
```

## 文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.zh-CN.md](docs/architecture.zh-CN.md) | 分层、密钥层级、密钥库格式、审计链、硬件抽象、密钥注入 |
| [docs/pkcs11.zh-CN.md](docs/pkcs11.zh-CN.md) | 机制、对象模型、厂商属性、密钥导入、KEM 操作、配置 |
| [hardware/README.md](hardware/README.md) | RTL 模块、验证策略、仿真器选择 |
| [demo/README.md](demo/README.md) | provider 演示与客户端库兼容性 |

## 构建

依赖：CMake ≥ 3.20、C11 编译器、OpenSSL 3、liboqs。

```bash
brew install liboqs openssl@3 cmake        # macOS
# Debian/Ubuntu：liboqs 从源码构建；其余用 libssl-dev、cmake、ninja-build
```

```bash
./tools/fetch_vectors.sh                   # 下载并展平 NIST ACVP 向量
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

可选组件是探测而非必需。没有 Verilator 时，仿真 RTL 后端不会被编入，
`accel_transport_verilator()` 返回 `NULL`；需要 `cocotb`、`iverilog` 或
`pkcs11-tool` 的测试会自行跳过并给出说明，而不是失败。

### 补充检查

```bash
./tools/rtl_sim.sh          # cocotb 回归（Icarus Verilog）
./tools/aarch64_test.sh     # 在 aarch64 Linux 容器中从零构建并回归
./tools/fuzz.sh             # libFuzzer 靶子（需要 LLVM clang）
./tools/profile.sh          # 采样剖析
./build/pqchsm-bench        # 算法级性能基线
./build/pqchsm-prim-bench   # 单次原语代价与实测 RTL 周期数
```

## 运行演示

两个演示都驱动共享库走完整生命周期：初始化令牌、设置 PIN、登录、在槽位中生成 ML-DSA
与 ML-KEM 密钥、签名与验签、读取属性、枚举对象。

```bash
cmake --build build --target pqchsm-p11

# Python，经 PyKCS11
python3 -m venv .venv-p11 && ./.venv-p11/bin/pip install -q PyKCS11
./.venv-p11/bin/python demo/python/pqchsm_demo.py

# Java，经 JDK 22+ 的 FFM API（无外部依赖）
java --enable-native-access=ALL-UNNAMED demo/java/PqcHsmDemo.java \
     "$PWD/build/pqchsm-pkcs11.dylib"
```

两者都直接使用低层 PKCS#11 绑定。**未**使用高层 provider 框架，因为它们尚不支持这些
机制——详情与验证程序见 [demo/README.md](demo/README.md)。

## 安全模型与局限

这是一个原型。以下是对它做什么、不做什么的准确陈述。

**安全边界是软件。** 运算期间明文密钥材料存在于进程内存中。调用方始终只看到句柄，
缓冲区会被清零并尽可能 `mlock`，但这一切都无法抵御能读取进程地址空间的攻击者。把
边界移入硬件是后续工作。

**密钥派生根是桩。** `src/crypto/kdr.c` 中是一个固定的 32 字节常量，其字面内容为
`PQC-HSM STUB KDR -- NOT SECRET!!`。在真实设备上，该值来自 eFUSE、BBRAM 或 PUF，且
永不出芯片。在此之前，设备绑定并不成立。

**RTL 覆盖的是两个算术核，不是密码加速器。** `ntt_core` 与 `keccak_f1600` 已实现并在
仿真中验证。采样、编码、压缩以及 ML-KEM/ML-DSA 的整体数据流仍全部由软件完成。完整
算法没有硬件实现。

**没有任何东西在真实硬件上运行过。** 没有开发板，没有跑过综合（`hardware/syn/` 中的
Vivado 脚本已写好但未验证），没有时序收敛、功耗测量、TRNG 熵评估、eFUSE、防拆检测，
也没有在内存压力下验证过 `mlock` 的实际行为。

**给不出加速比。** 在开发机上，仿真核 @100 MHz 慢于 liboqs 的手写 NEON 汇编。该比较
本身不成立——目标平台是无 SIMD 的 Cortex-A53——但这确实意味着本项目目前无法给出加速
数字。`./build/pqchsm-prim-bench` 会打印实测数据与相应说明。

**其它已知缺口。** 审计模块假设单写者，不对文件加锁。Shamir 分片校验和无密钥，能检出
损坏而非篡改。SO PIN 失败只计数不锁定槽位——锁定会使设备变砖，兜底手段是 M-of-N
恢复。`CKM_HASH_ML_DSA_*` 未实现，因此 PKCS#11 的多段签名会缓冲整条消息，而不是流式
推进摘要。

## 测试

| 检查项 | 结果 |
|---|---|
| `ctest` | 38 / 38 |
| 断言总数 | 约 3700 |
| NIST ACVP 向量 | 390 条逐字节通过，60 条显式跳过 |
| ASan + UBSan | 38 / 38 |
| ThreadSanitizer | 0 竞争（以移除锁作反证：报告 9 处） |
| macOS `leaks` | 0 泄漏 |
| libFuzzer | 138 万次执行，无崩溃 |
| aarch64 Linux（GCC 12） | 38 / 38 |
| cocotb RTL 回归 | 5 个模块共 14 个测试 |

测试源码中贯穿两条做法：

- **独立预言机。** 不因为结果与本项目自己的模型一致就认为它正确。KMAC 对照 NIST 文档、
  OpenSSL 以及另一份独立的 Keccak；NTT 对照完全不触碰旋转因子表的 schoolbook 负循环
  卷积，并通过重建 ML-KEM 密钥生成、逐字节重现 ACVP 的 `ek`/`dk` 来验证；Keccak 对照
  公开的全零置换向量以及 `hashlib`/OpenSSL。
- **反证。** 通过刻意破坏并确认测试失败来验证断言本身有效——扰动一个旋转因子、丢掉一层
  NTT、翻转 Keccak 轮常数的一个比特、在 TSan 下移除一把锁、加入一个假的密钥读回函数。

## 路线图

不依赖硬件的软件工作已基本完成。余下的部分需要硬件：

1. 用 RTL 实现完整的 ML-KEM / ML-DSA 核（采样、编码、数据流），通过现有寄存器接口逐个
   模式替换软件桩。
2. 在目标器件上完成综合与时序收敛；`hardware/syn/` 中的 out-of-context 脚本已就位但
   从未运行。
3. 将密钥派生根移入 eFUSE/BBRAM/PUF，并把安全边界移入可编程逻辑。
4. 用环形振荡器 TRNG 加 SP 800-90B 熵评估替换 `RAND_bytes`。
5. 在目标平台上测得端到端加速比——这是唯一能给出真实数字的地方。

每一步都通过已经存在的寄存器接口，把一个软件模式换成硬件模式，其上各层不受影响。

## 许可

[Apache-2.0](LICENSE)。选择它而非 MIT，是因为它带有显式的专利授权与专利报复终止条款
（第 3 条）；专利在这个领域是真实存在的风险面，而 MIT 对此完全沉默。

`third_party/pkcs11-v3.2/` 下的三个头文件是 OASIS 文档，按其自身条款原样收录。
