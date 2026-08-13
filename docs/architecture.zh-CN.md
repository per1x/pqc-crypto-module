[English](architecture.md) · **中文**

# 架构

本文档描述该模块如何组织，以及各层边界为何落在当前位置。构建与使用说明参见
[README](../README.zh-CN.md)；PKCS#11 接口参见 [pkcs11.zh-CN.md](pkcs11.zh-CN.md)。

## 分层

```
   PKCS#11 v3.2 shared library          Daemon, CLI, admin tool
   src/p11/                             cli/
 ─────────────────────────────────────────────────────────────────
   slot manager     keystore        backup / recovery    audit
   src/slot/        src/store/      src/backup/          src/audit/
 ─────────────────────────────────────────────────────────────────
   pqc_backend_t          include/pqchsm/pqc.h
 ─────────────────────────────────────────────────────────────────
   accel_transport_t      include/pqchsm/accel.h
   stub | Verilator RTL | AXI | /dev/mem + mmap
```

每一层只依赖其下方的接口。上层全部基于不透明句柄编写，因此后端可以更换，而无需任何
调用方随之改动。

## 句柄与 API 面

`include/pqchsm/` 中声明的函数没有任何一个返回私钥材料。密钥生成返回
`hsm_handle_t`；签名接收一个句柄和一条消息；解封装接收一个句柄和一段密文。公钥可以
跨越 **API 面**，私钥永远不会。（这是主机 API 的性质。模块的**密码边界**是另一个、
更低的东西 —— 可编程逻辑；见 [security-policy.zh-CN.md](security-policy.zh-CN.md)。）

句柄中编码了槽位标识符与该槽位的世代计数器。销毁对象会使世代递增，因此该槽位此前
发出的每一个句柄都立即失效，而不会悄悄指向一把新密钥。

## 槽位模型

槽位/令牌/对象/会话模型紧贴 PKCS#11，因为该模块的预期用法就是经由 PKCS#11 使用，
一一对应可以省去一层阻抗适配。

每个槽位至多持有一对密钥，与嵌入式目标下适度的单槽位存储预算相称。因此一个已加载的
槽位恰好呈现两个 PKCS#11 对象，其句柄可以互相推导，无需额外的对象表。

槽位状态由显式的状态转换表表达，而不是散落各处的条件判断：

```
UNINIT ──InitToken──► INITIALIZED ──Generate/LoadSeed──► LOADED ──InUse──► IN_USE
   ▲                       │                               │                 │
   └────────────── Zeroize (reachable from any state, irreversible) ──────────┘
```

访问控制基于角色（`SO`、`USER`），由槽位管理器而非 PKCS#11 层强制执行，因此 CLI 与
守护进程受到同一套约束。

## 密钥层级

```
KDR  (device-bound root, 32 bytes)
 └── KEK        wraps key material at rest; cannot leave the device
RMK  (recovery master key, randomly generated)
 └── BEK        wraps backup blobs; portable to a replacement device
      └── Shamir M-of-N shares over GF(256)
```

存在两把不同的包裹密钥，是因为两种用途本身互相冲突。以设备绑定密钥包裹的材料是安全
的，但设备失效后无法恢复；以可迁移密钥包裹的材料可以恢复，其保护强度却完全取决于
分片的保管。每个槽位带有一个策略位，控制其密钥是否参与备份，因此设备身份密钥可以被
有意设定为不可恢复。

Shamir 拆分使用常数时间的 GF(256) 乘法。分片完整性由一段无密钥的哈希前缀校验，可以
检出损坏；权威的完整性判据是重组之后的 AES-GCM 标签。

每个槽位还会在其包裹后的 blob 中存放一把随机生成的 PIN 验证密钥，而不是从设备根派生
验证值。若从根派生，恢复出来的令牌将无法在替换设备上完成认证。

## 密钥库格式

密钥库文件由逐槽位的 blob 序列加上一个整文件 MAC 构成：

```
slot blob := plaintext metadata ‖ AES-256-GCM(KEK, aad = metadata, key material ‖ PIN material)
file      := header ‖ slot blob × N ‖ KMAC256(file)
```

元数据以附加认证数据的形式参与认证而非加密，因此槽位的算法、用途与策略无需解包即可
读取，同时保持防篡改。

写入是原子的：`write to temporary file → fsync → rename → fsync(directory)`。崩溃之后
留下的要么是旧文件，要么是新文件，绝不会是残缺的文件。

## 审计链

每次状态转换都会追加一条记录，其 SHA3-256 哈希覆盖前一条记录的哈希。任何改动都会
因此向后传播并可被发现——前提是存在一个参照点。

纯哈希链无法抵御能够重写整个文件的攻击者，因为对方可以重算每一个哈希。为此，链头会
用 ML-DSA 设备身份密钥签名并锚定到设备之外。验证时将重算出的链头与最后一个锚定签名
进行比对。

该模块假设单写者，不对审计文件加锁。

## 硬件抽象

`include/pqchsm/accel.h` 定义了一套 AXI 风格的寄存器表：

| 偏移 | 寄存器 | 访问 | 用途 |
|---|---|---|---|
| 0x00 | `CTRL` | W | `START`、`SOFT_RESET` |
| 0x04 | `STATUS` | R | `DONE`、`BUSY`、`ERR` |
| 0x08 | `MODE` | RW | 操作码 |
| 0x0C | `PARAM` | RW | 参数集 |
| 0x10 | `IN_LEN` | RW | 输入长度 |
| 0x14 | `OUT_LEN` | R | 输出长度，由加速器写入 |
| 0x18 | `ERRCODE` | R | 错误详情 |

批量数据不经由寄存器搬运；transport 另外暴露 `write_data`/`read_data` 入口，对应真实
硬件上的 DMA。

三种 transport 以完全相同的方式实现该接口：

- **软件桩** — 纯软件，调用 liboqs。始终可用。
- **Verilator** — 驱动仿真 RTL。实现 NTT 与 Keccak 模式；其余任何模式都返回明确的
  "不支持"错误，而不是回退到软件，因此 RTL 路径的覆盖范围不会被夸大。
- **AXI** — 同一份 RTL，经真实的 AXI4-Lite 与 AXI4-Stream 事务驱动。
- **mmap** — 在真实可编程逻辑上经由 `/dev/mem` 访问。代码已存在
  （`src/hal/accel_mmap.c`），基址在构建时给定。

> **板上真正跑过的是哪一条、没跑过的是哪一条。** 硬件密码引擎在真硅上是由
> `board/` 下的独立程序驱动的——它们 `mmap` `/dev/mem` 到 `0x8000_0000` 直接操作
> 各个核。ML-KEM 的 ACVP 结果、对称向量、TRNG 熵采集与边界证明都来自那里。
> `accel_transport_t` 之上的 `src/` 那一栈**没有**对着真实可编程逻辑驱动过，
> 把它接上去是另一件事。

软件桩与仿真 RTL 必须经由该接口产出逐字节一致的结果，这一点由
`tests/unit/test_accel.c` 断言。

### SHA3 与 SHAKE

`accel_shake()` 在 C 侧实现海绵结构——填充、速率（rate）、吸收、挤压——仅在
Keccak-f[1600] 置换处调用 transport。只做置换的核面积更小，且能同样服务于 SHAKE128、
SHAKE256、SHA3-256 与 SHA3-512；分帧属于处理器一侧的职责。由此，整条 SHA3/SHAKE
路径都可以跑在仿真 RTL 上并与 OpenSSL 比对。

## 密钥注入

在不将密钥暴露于链路的前提下开通密钥，依靠的是模块自身的 KEM：

```
provisioning host                                device
─────────────────                                ──────
obtain device ML-KEM public key  ◄────────────── public key of a KEM slot
(ct, CEK) = Encaps(ek)
blob = header ‖ ct ‖ AES-GCM(CEK, header, seed)
                                 ──────────────► Decaps(dk, ct) recovers CEK
                                                 unwrap seed, load into target slot
```

链路上只有密文；事后双方都会将会话密钥清零。注入的是种子，而不是展开后的私钥：
FIPS 203 与 204 的密钥完全由其种子决定，因此这样传输的敏感字节更少，并且让设备自己
验证展开结果。

覆盖一个已加载的槽位要求该槽位的策略允许注入，因此开通操作不会悄悄顶替一把本不该被
替换的密钥。

## 随机数

随机字节取自 OpenSSL 的 `RAND_bytes`，经由一层间接调用，以便由硬件熵源替换。确定性
测试向量由专门的去随机化入口（`pqc_keypair_from_seed`、`pqc_encaps_derand`）处理，
而不是替换随机源，因此生产路径在运行期永远不可重新配置。

## 目录索引

| 路径 | 内容 |
|---|---|
| `include/pqchsm/` | 公共头文件；全部 API 面 |
| `src/crypto/` | liboqs 绑定、KDF、密钥派生根 |
| `src/slot/` | 槽位状态机、会话、元数据、持久化 |
| `src/store/` | AES-256-GCM 包裹、密钥库文件格式 |
| `src/backup/` | Shamir 拆分、备份与恢复、密钥注入 |
| `src/audit/` | 哈希链与 ML-DSA 锚定 |
| `src/hal/` | 加速器抽象与各 transport |
| `src/p11/` | PKCS#11 v3.2 共享库 |
| `src/proto/` | TLV 命令协议 |
| `src/util/` | 安全清零、锁页分配、KAT 解析 |
| `cli/` | 守护进程、客户端 CLI、管理工具 |
| `hardware/` | RTL、测试台、参考模型、综合脚本 |
| `tools/` | 向量获取、基准测试、回归脚本 |
