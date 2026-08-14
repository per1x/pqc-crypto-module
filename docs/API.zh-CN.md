[English](API.md) · **中文**

# API

同一套硬件之上提供两个前端：一个是 **SDF 风格 C 库**（`service/`），经安全世界
与 FPGA 核对话；另一个是主机软件栈之上的 **PKCS#11 v3.2 模块**（`src/p11/`）。

- [分层](#分层)
- [SDF 风格接口](#sdf-风格接口)
- [与 GM/T 0018 的对应](#与-gmt-0018-的对应)
- [PKCS#11 v3.2](#pkcs11-v32)
- [私钥在哪里](#私钥在哪里)

## 分层

```
┌──────────────────────────────────────────────────────────────┐
│ application        business code, standard interfaces only   │
├──────────────────────────────────────────────────────────────┤
│ libsdfe / pqchsm-pkcs11.so   standard API → internal wire    │
│                              stateless, safe across processes│
├──────────────────────────────────────────────────────────────┤
│ pqchsm_fpgad       sessions, handles, ACL, serialisation     │
├──────────────────────────────────────────────────────────────┤
│ /dev/secmmio       misc device turning requests into SMCs.   │
│                    NOT a trust boundary — a courier          │
├──────────────────────────────────────────────────────────────┤
│ BL31 SiP (EL3)     whitelist, and the only thing that can    │
│ AxPROT[1] = 0      issue a secure transaction  ◀── boundary  │
├──────────────────────────────────────────────────────────────┤
│ FPGA cores         AXI firewall → trng / key_vault / sym /   │
│ SECURE_ONLY=1      mlkem. Non-secure access refused at the bus│
└──────────────────────────────────────────────────────────────┘
```

每一层为什么必须存在：

| 层 | 解决什么问题 | 没有它会怎样 |
|---|---|---|
| 标准库 | 应用换模块不必改代码 | 每个应用都得自己拼装线协议 |
| 守护进程 | 多个进程共享**同一块**硬件：会话、句柄、串行化 | 两个进程同时驱动 `mlkem_axi`，互相踩对方的寄存器 |
| 内核节点 | 用户空间发不了 SMC | 命令逻辑就得搬进内核 |
| EL3 SiP | 普通世界的事务总是带着 `AxPROT[1] = 1` 而被拒 | 在 `SECURE_ONLY=1` 的构建里，这些核根本没法用 |
| PL 防火墙 | 门控本身 | 没有边界——而且 PS 侧没有任何东西能顶替它 |

守护进程是最常被认为可以砍掉的一层。它砍不得：三个核都是有状态的序列——写寄存器、
启动、轮询、读结果——两个进程在同一个核上交错，产出的结果**彼此错位、单看却都
说得通**。串行化需要一个单点，而这个单点必须记住会话。

## SDF 风格接口

`service/sdfe.h`。前缀是 `SDFE_`（"SDF Extended"），不是 `SDF_`：命名沿用
GM/T 0018 的惯例，但国密标准尚未定义 ML-KEM 接口，因此这些名字不冒充标准函数名。

**这个库不做任何密码运算。** 它把一次调用翻译成一条发给 `pqchsm_fpgad` 的请求。
它是无状态的；不同进程可以各自链接它、各自开会话，永远看不到对方的句柄。

```c
/* 设备与会话 */
int SDFE_OpenDevice(SDFE_HANDLE *phDev);
int SDFE_CloseDevice(SDFE_HANDLE hDev);
int SDFE_OpenSession(SDFE_HANDLE hDev, SDFE_HANDLE *phSession);
int SDFE_CloseSession(SDFE_HANDLE hSession);
int SDFE_GetDeviceInfo(SDFE_HANDLE hSession, char *buf, size_t cap);

/* 随机数——来自 PL 里的环形振荡器源，不是软件 PRNG */
int SDFE_GenerateRandom(SDFE_HANDLE hSession, uint32_t len, uint8_t *out);

/* ML-KEM——dk 留在守护进程；应用拿到的是句柄 */
int SDFE_GenerateKeyPair_MLKEM(SDFE_HANDLE hSession, uint32_t pset,
                               uint8_t *ek, uint32_t *ek_len,
                               uint32_t *key_handle);
int SDFE_Encapsulate_MLKEM(SDFE_HANDLE hSession, uint32_t pset,
                           const uint8_t *ek, uint32_t ek_len,
                           uint8_t *ss, uint32_t *ss_len,
                           uint8_t *ct, uint32_t *ct_len);
int SDFE_Decapsulate_MLKEM(SDFE_HANDLE hSession, uint32_t key_handle,
                           const uint8_t *ct, uint32_t ct_len,
                           uint8_t *ss, uint32_t *ss_len);

/* 对称——密钥写入 key_vault，此后按槽位使用 */
int SDFE_ImportKey(SDFE_HANDLE hSession, uint32_t slot,
                   const uint8_t *key, uint32_t key_len);
int SDFE_Encrypt(SDFE_HANDLE hSession, uint32_t alg, uint32_t slot,
                 const uint8_t *in, uint8_t *out);   /* 单个 16 字节分组 */
int SDFE_Decrypt(SDFE_HANDLE hSession, uint32_t alg, uint32_t slot,
                 const uint8_t *in, uint8_t *out);

const char *SDFE_StrError(int rv);
```

返回码沿用 SDF 惯例：成功为 `SDR_OK`（0），否则为 `SDR_BASE + n`——
`SDR_OPENDEVICE`、`SDR_COMMFAIL`、`SDR_INARGERR`、`SDR_KEYNOTEXIST`、
`SDR_HARDFAIL`、`SDR_UNKNOWERR`。

参数集为 `SDFE_MLKEM_512` / `_768` / `_1024`；对称算法为 `SDFE_ALG_AES128` /
`_AES256` / `_SM4`，与 `sym_axi` 的 `ALG` 字段对应。

`service/sdf_demo.c` 是一个完整示例，其中不含任何硬件细节——没有寄存器、没有
`/dev/mem`、没有 SMC——并且**只**链接 `libsdfe`，不链接密码库。它自己算不出任何
东西；它打印出的每一个正确结果都来自 FPGA。见
[USAGE.zh-CN.md](USAGE.zh-CN.md#运行-sdf-演示)。

## 与 GM/T 0018 的对应

| SDF 函数 | 对应到 | 状态 |
|---|---|---|
| `SDF_OpenDevice` / `CloseDevice` | 连接/断开守护进程套接字 | 已具备 |
| `SDF_OpenSession` / `CloseSession` | `CMD_SESSION_OPEN` / `_CLOSE` | 已实现 |
| `SDF_GetDeviceInfo` | 静态能力表 + 槽位信息 | 已具备 |
| `SDF_GenerateRandom` | 经 EL3 读 `trng_axi` 的 `RDATA` | **硬件** |
| `SDF_Encrypt` / `SDF_Decrypt`（SM4） | `sym_axi` + `key_vault` 槽位 | **硬件** |
| `SDF_HashInit/Update/Final`（SM3） | `sym_axi` 的 SM3 通路 | **硬件** |
| `SDF_GenerateKeyPair_ECC`（SM2） | — | ❌ **没有 SM2 核** |
| `SDF_InternalSign_ECC` / `InternalVerify_ECC` | — | ❌ 没有 SM2 核 |
| `SDF_ImportKeyWithISK_ECC` 及密钥交换一族 | 依赖 SM2 | ❌ 没有 SM2 核 |
| `SDF_ExportSignPublicKey_ECC` | 公钥可以出去；私钥永不 | 结构上成立 |

SDF 中*内部密钥*的概念天然对应 `key_vault`：`SDF_InternalSign` 接受的是设备内部
密钥索引（`uiISKIndex`），从不携带私钥，这正是"按槽位或句柄引用，永不离开硬件"。

把差距说清楚：本设计覆盖的是 SDF 的**对称、杂凑与随机数**服务。非对称一族
（SM2/ECC）在这里**没有硬件**，而对国密送检来说，这是一块相当大的缺口。

**ML-KEM 目前还没有国标接口**，因此有两条路可走。本项目把 **PKCS#11 v3.2 的 PQC
机制当作主路径**——国际标准已经定稿，头文件已随仓库引入，`C_Encapsulate`/
`C_DecapsulateKey` 已实现——同时把 `SDFE_*` 这套名字保留为同一批线上命令之上的
一层薄封装，这样等国标真正落地时，改动只局限在那一层。

## PKCS#11 v3.2

`src/p11/p11_module.c` 实现了 44 个 `C_` 函数。用
`cmake --build build --target pqchsm-p11` 构建。

```c
CK_INTERFACE *iface;
C_GetInterface(NULL, NULL, &iface, 0);      /* v3.2 的机制在这里 */
```

| 机制 | 由什么支撑 | 状态 |
|---|---|---|
| `CKM_ML_KEM`（0x17） | `mlkem_axi`，三个参数集全部支持 | **硬件** |
| `CKM_AES_ECB` / `CKM_AES_CBC` | `sym_axi` 的 AES-128/256 | **硬件** |
| `CKM_SHA3_256` | `sha3_core`（目前只能经 ML-KEM 到达） | 需要一条专用通路 |
| `CKM_ML_DSA`（0x1D） | ML-DSA 算子，未串成核 | ❌ 仅软件 |
| SM4 / SM3 | 不存在标准机制码 | 厂商自定义码 |

`C_GenerateKeyPair` → `CMD_GENERATE` → `mlkem_axi` KeyGen；
`C_DecapsulateKey` → `CMD_DECAPS` → `mlkem_axi` Decaps。

对象模型紧贴 PKCS#11，因为一一对应可以省掉一层阻抗匹配。每个槽位最多持有一对
密钥，因而恰好呈现两个对象，两者的句柄可以互相推出；不需要单独的对象表。访问
控制基于角色（`SO`、`USER`），在槽位管理器而不是 PKCS#11 层执行，因此命令行与
守护进程得到的是完全一致的约束。

已知偏差：`CKM_HASH_ML_DSA_*` 未实现，因此多段签名会把整条消息缓存下来，而不是
流式计算摘要。上层 provider 框架（JCA、PyCryptodome 之流）尚不支持这些机制；
`demo/` 里的演示因此直接使用低层绑定，`demo/java/SunP11Probe.java` 演示了为什么。

## 私钥在哪里

有两种情形，不能揉成一句话。

| 密钥 | 存在哪里 | 会离开硬件吗？ |
|---|---|---|
| 对称（AES / SM4） | `key_vault` 寄存器阵列 | **不会。** RTL 里没有总线侧的读出路径。演示程序在导入后擦掉自己那份副本，此后没有任何人——包括守护进程——能把它取回来 |
| ML-KEM `dk` | 目前由 `KeyGen` 返回；由守护进程持有 | ⚠️ **会，它会离开硬件**——但不会离开*接口*。应用手里始终只有一个句柄 |

第二行是当前状态，不能简化成"私钥永不离开硬件"。要让那句话成立，需要在
`mlkem_axi` 内部实现私钥存储与按句柄使用，那是产品级的工作，不是原型级的。见
[SECURITY.zh-CN.md](SECURITY.zh-CN.md)。
