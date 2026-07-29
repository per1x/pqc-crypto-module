[English](pkcs11.md) · **中文**

# PKCS#11 接口

该模块构建为 `pqchsm-pkcs11.dylib` / `.so`，是一个 PKCS#11 v3.2 provider，通过该版本
原生定义的机制暴露 ML-KEM 与 ML-DSA。

```bash
cmake --build build --target pqchsm-p11
```

## 获取函数表

有两个入口，二者均可使用：

```c
CK_FUNCTION_LIST_PTR f;
C_GetFunctionList(&f);                       /* 2.40-shaped table */

CK_INTERFACE_PTR iface;
C_GetInterface(NULL, NULL, &iface, 0);       /* "PKCS 11" v3.2 table */
CK_FUNCTION_LIST_3_2_PTR f32 = iface->pFunctionList;
```

`CK_FUNCTION_LIST` 是 2.40 的结构体，不含 v3.x 新增函数所需的字段。
**`C_EncapsulateKey` 与 `C_DecapsulateKey` 只能经由 `C_GetInterface` 访问。** 两张表
指向的是同一批实现。

`C_GetInterface` 接受 `NULL` 或 `"PKCS 11"` 作为接口名。请求
`CKF_INTERFACE_FORK_SAFE` 会返回 `CKR_FUNCTION_FAILED`：该模块不是 fork-safe 的，也
不会声称自己是。

## 机制

| 机制 | 代码 | 操作 |
|---|---|---|
| `CKM_ML_KEM_KEY_PAIR_GEN` | 0x16 | 密钥对生成 |
| `CKM_ML_KEM` | 0x17 | 封装、解封装 |
| `CKM_ML_DSA_KEY_PAIR_GEN` | 0x1C | 密钥对生成 |
| `CKM_ML_DSA` | 0x1D | 签名、验签 |

参数集通过 `CKA_PARAMETER_SET` 选择：

| 属性取值 | 算法 |
|---|---|
| `CKP_ML_KEM_512` / `CKP_ML_KEM_768` / `CKP_ML_KEM_1024` | ML-KEM |
| `CKP_ML_DSA_44` / `CKP_ML_DSA_65` / `CKP_ML_DSA_87` | ML-DSA |

`CKM_HASH_ML_DSA_*` 未实现。

## 已实现的函数

通用类、槽位与令牌管理、会话、对象、签名、验签、密钥对生成，以及 KEM 操作：

```
C_Initialize          C_Finalize            C_GetInfo             C_GetFunctionList
C_GetInterface        C_GetInterfaceList    C_GetSlotList         C_GetSlotInfo
C_GetTokenInfo        C_GetMechanismList    C_GetMechanismInfo    C_InitToken
C_InitPIN             C_OpenSession         C_CloseSession        C_CloseAllSessions
C_GetSessionInfo      C_Login               C_Logout              C_CreateObject
C_DestroyObject       C_GetAttributeValue   C_FindObjectsInit     C_FindObjects
C_FindObjectsFinal    C_GenerateKeyPair     C_SignInit            C_Sign
C_SignUpdate          C_SignFinal           C_VerifyInit          C_Verify
C_VerifyUpdate        C_VerifyFinal         C_EncapsulateKey      C_DecapsulateKey
```

未实现的函数在函数表中保留为 `NULL` 项，而不是返回错误的桩函数，因此调用方可以通过
检查表项判断能力。

## 对象模型

每个槽位至多持有一对密钥，因此一个已加载的槽位恰好呈现两个对象：一把私钥和对应的
公钥。二者的句柄可以互相推导。

对公钥取 `CKA_VALUE` 返回编码后的公钥。对私钥取 `CKA_VALUE` 一律返回
`CKR_ATTRIBUTE_SENSITIVE`——不存在导出私钥材料的代码路径。

`C_FindObjectsInit` 按 `CKA_CLASS` 过滤，模板中的其它属性被忽略。会话绑定到单个槽位，
因此查找只会返回该槽位的对象。

### 厂商属性

PKCS#11 没有标准属性可以表达"这把密钥可以进入备份"。`CKA_EXTRACTABLE` 表达的是另一
回事——密钥能否以明文导出，而本模块从不允许这样做。为此改用两个厂商定义的属性：

| 属性 | 默认值 | 含义 |
|---|---|---|
| `CKA_PQCHSM_BACKUPABLE`（`CKA_VENDOR_DEFINED \| 0x01`） | `CK_TRUE` | 密钥可以进入 M-of-N 备份 |
| `CKA_PQCHSM_SEED_STORAGE`（`CKA_VENDOR_DEFINED \| 0x02`） | `CK_FALSE` | 存储种子并按需展开，而不是存储展开后的密钥 |

`CKA_BACKUPABLE` 默认为真，因为密钥永远无法备份的令牌会使整套恢复机制失去意义。对于
应当随设备一同消失的密钥（例如设备身份密钥），需显式设为 `CK_FALSE`。

## 密钥导入

`C_CreateObject` 接受：

- **由种子导入私钥** — `CKA_CLASS = CKO_PRIVATE_KEY`、`CKA_KEY_TYPE`、
  `CKA_PARAMETER_SET` 以及 `CKA_SEED`（ML-KEM 为 64 字节，ML-DSA 为 32 字节）。
- **会话对称密钥** — `CKA_CLASS = CKO_SECRET_KEY` 配合 `CKA_VALUE`。

为私钥提供 `CKA_VALUE` 会返回 `CKR_ATTRIBUTE_TYPE_INVALID`。不存在将展开后的私钥载入
槽位的路径；密钥只能通过内部生成或种子进入。由于 FIPS 203 与 204 的密钥完全由其种子
决定，这并不削弱能力，而且传输的敏感字节更少。若需要在不暴露种子的情况下开通密钥，
使用 [architecture.zh-CN.md](architecture.zh-CN.md#密钥注入) 中描述的密钥注入协议。

## 多段签名

`C_SignUpdate` 累积消息，`C_SignFinal` 一次性完成签名。ML-DSA 不是先哈希后签名的
构造：FIPS 204 以域分隔符与上下文前缀对整条消息计算 `μ`，而拒绝采样循环的每次重试都
需要 `μ`，因此消息无法增量消费。内存占用由此与消息长度成正比。以流式方式推进摘要则
需要 `CKM_HASH_ML_DSA_*`，该机制未实现。

`C_VerifyUpdate` / `C_VerifyFinal` 的行为与之对称。

## KEM 操作

```c
CK_MECHANISM m = { CKM_ML_KEM, NULL, 0 };
CK_ULONG ctlen = sizeof(ct);
CK_OBJECT_HANDLE shared;

f32->C_EncapsulateKey(session, &m, publicKey, tmpl, n, ct, &ctlen, &shared);
f32->C_DecapsulateKey(session, &m, privateKey, tmpl, n, ct, ctlen, &shared);
```

两者产出的都是持有共享密钥的**会话对象**，而不是原始字节，这正是规范所规定的。该对象
不会持久化，会话关闭时即被销毁。

共享密钥默认为 `CKA_SENSITIVE` 且非 `CKA_EXTRACTABLE`，因此读取 `CKA_VALUE` 返回
`CKR_ATTRIBUTE_SENSITIVE`。若需要读出——例如用于展示双方结果一致——在模板中设置
`CKA_EXTRACTABLE = CK_TRUE` 与 `CKA_SENSITIVE = CK_FALSE`。

传入长度不正确的密文会返回 `CKR_ENCRYPTED_DATA_LEN_RANGE`。而被破坏的密文**不会**
产生错误：ML-KEM 的隐式拒绝会给出一个确定但不同的共享密钥。

## 配置

该模块将状态保存在密钥库文件中，因为每个 PKCS#11 客户端都可能是独立进程：

| 变量 | 默认值 | 含义 |
|---|---|---|
| `PQCHSM_KEYSTORE` | `$HOME/.pqchsm/keystore.bin` | 密钥库路径 |
| `PQCHSM_SLOTS` | 4 | 槽位数量 |

`C_Initialize` 加载密钥库，此后每个改变状态的操作都会立即将其写回。

## 行为说明

- 对已初始化的令牌调用 `C_InitToken` 返回 `CKR_ACTION_PROHIBITED`。部分实现会重新
  初始化；那会销毁全部内容，因此必须先通过管理工具执行一次显式清零。
- `C_CloseAllSessions` 只关闭指定槽位上的会话。
- `C_InitToken` 的 `pLabel` 必须恰好 32 字节，以空格填充且不以 NUL 结尾，符合规范
  要求。
- SO PIN 失败会递增计数器，但不会锁定槽位；锁定会使设备无法使用，M-of-N 恢复才是
  预期的兜底手段。

## 客户端支持

高层 provider 框架尚不支持这些机制。请使用直接传递机制代码的低层绑定——Python 用
PyKCS11，Java 用 JDK FFM API 或封装库。[demo/README.md](../demo/README.md) 记录了具体
的缺口，并为两者提供了可运行的示例。
