# 对称密钥归 TEE 管（D7 的「密钥装载」那一半）—— 方案与结果

分支 `feature/tee-symkey`（基于合并了批 2 的 `main`）。

> 本文 §1–§3 是**动手之前**写下的方案与取舍；§4 起是做完之后的结果。
> 板子这一轮不在：所有需要上板的验证都列在 §6 的清单里，**没有一条被标成"已验"**。

---

## 1. 问题

批 2 之后 PQC 那一侧已经是"种子归 TA、PL 无状态"，但对称那一侧还停在批 1 之前：

```
应用 ──明文 key──▶ daemon（普通世界进程内存）──PL_WR / mmio──▶ key_vault KEY_IN
```

- 对称密钥的**明文**在普通世界 daemon 的栈上待过一趟；
- 演示位流（`SECURE_ONLY=0`）下普通世界经 `/dev/mem` 直接写 `KEY_IN`；
- 送检位流下 PL 只认安全事务，但 **BL31 的通用 `PL_WR` 白名单放行槽 1** ——
  root 经 SiP 转一手照样能写，PL 那道门白做（与种子口当年的 CODE-1 同形）。

这正是 `STATELESS-PL-CONFLICTS §7.1` 里写下、却一直没做的三件事。

## 2. 方案（按 §7.1 的三道防护）

| 层 | 改动 |
|---|---|
| **PL** `key_vault_axi` | 装载口（`SLOT_SEL` / `KEY_IN` / `SLOT_CTRL` 全部动作）**只认 `AxPROT[1]=0`，与 `SECURE_ONLY` 无关**——与种子口同一口径。被拒的写 RAZ/WI + 计数（新寄存器 `0x34 LOAD_REJ`），不回 SLVERR（理由同种子口：posted 写的错误是 SError）。`CTRL.ZEROIZE` **不**门控：擦除永远是安全方向 |
| **PL** `sym_axi → key_vault` | `SY_CTRL.ZEROIZE` 新增一根线直通金库的 zeroize。于是普通世界在**不碰槽 1** 的前提下仍能执行"段末即擦" |
| **EL3** `patch_atf_secmmio.py` | 槽 1 从通用 `PL_RD` / `PL_WR` **整槽排除（读写都拒、两个世界都拒）**；新增 SiP `0x8200ff16 PQC_SYMKEY`，**无条件只认安全世界**，只有六个窄动作：选槽+BEGIN、写一个字、COMMIT、ERASE、ZEROIZE、读 `SLOT_STAT`。**没有 LOCK**（见 §3.3） |
| **S-EL1** PTA | `pqchsm_seed.pta` 加第二条命令转发 `PQC_SYMKEY`，两道门（拒普通世界直开、只认用户 TA）不变 |
| **TA** | 新增三条命令：`CMD_SYMKEY_NEW`（TA 生成 → 送进槽 → 包成 PWRP blob 交回）、`CMD_SYMKEY_LOAD`（blob → 解开 → 送进槽）、`CMD_SYMKEY_KAT`（按编号装一把**公开**测试向量密钥，只许进槽 7）。三条都**不回任何明文字节** |
| **daemon / libsdfe** | 新线格式操作 `OP_SYMKEY_GEN` / `OP_SYMKEY_LOAD` / `OP_SYMKEY_KAT`，daemon 只搬 blob；`OP_IMPORT_KEY`（明文导入）**拒绝**，回 `SDR_NOTSUPPORT`；`session_end` 发的 `SY_CTRL.ZEROIZE` 现在连金库一起擦 |

分组运算路径**一行不动**：`sym_axi`（槽 2）仍对普通世界开放，daemon 照旧写
`KEY_SLOT` / `DIN` / `CMD`、读 `DOUT`，密钥仍经 use 口的 256 根线进核。
**只有装载那一步搬进了安全世界**。

### 2.1 性能：只有 provisioning 走 SMC

`§7.1` 担心的"逐分组走 SMC 吞吐腰斩"不会发生：

- 一次装载 = 1 次 TA 调用 ≈ 1 + 8 + 1 + 1 = **11 次 PTA→SMC**，每段只发生一次；
- 分组路径的寄存器访问数**与改动前完全相同**（每块约 10 次，送检形态下经 `PL_RD/PL_WR`，演示形态下 mmio）；
- 对外口径因此是："权威副本在 TA，PL 是**单段使用容器**、段末即擦"。

## 3. 取舍（动手前定下的）

### 3.1 明文导入：拒，不做"转运密钥"

要做到"daemon 不碰明文"，**明文导入这个动作本身就不能存在于 daemon 的接口上**——
否则明文必然在 daemon 的缓冲区里走一趟。可选的两条路：

| | 做法 | 取舍 |
|---|---|---|
| A | TA 暴露一把临时 ML-KEM 转运公钥，客户端封装后送密文 | 真正的"导入"，但客户端（libsdfe 是无状态薄库）要带 ML-KEM + AEAD，接口与测试面都大一圈 |
| **B（选）** | 只提供**生成**与**按 blob 重装**；明文导入回 `SDR_NOTSUPPORT` | 与业界信封加密一致（数据密钥由 HSM 生成并包裹）；代价是不能导入外部已有的对称密钥 |

选 B。A 记为将来项（需要导入外部密钥时再做，形状已经清楚）。

### 3.2 KAT：公开向量表，只许进槽 7

去掉明文导入之后，"用标准向量证明 AES/SM4 算对了"需要另一条路。做法：TA 里编译一张
**公开测试向量密钥表**（FIPS-197 / SP 800-38A / GB/T 32907），调用方只给**编号**。
线上仍然没有一个明文密钥字节。

但"装一把人人都知道的密钥"本身就是**密钥替换**：若它能进任意槽，root 可以在受害者
装载之后把同一槽换成已知密钥。所以 **KAT 只许进槽 7**，`NEW` / `LOAD` 只许进槽 0–6。
产品路径用不到槽 7，KAT 路径也够不到产品槽。

### 3.3 不给 LOCK

`key_vault` 有一次性 `LOCK`（锁上之后不能写也不能擦，只有 zeroize/复位能清）。
在"PL 是单段使用容器"的模型里，LOCK 的语义是"让这把密钥在 PL 里过夜"——
与 V-04 删 `dk_lock/sk_lock` 同理，**守在与模型相反的方向**。EL3 那条 SiP 里
就没有 LOCK 动作；PL 侧 LOCK 也一并收进"只认安全事务"。RTL 保留这个能力
（删它要动 `key_vault.v` 的核心状态机，不在本轮范围）。

### 3.4 blob 的域分隔

对称 blob 与种子 blob 用同一个 KEK。为防"把一份 32 字节的 ML-DSA ξ blob 当成
AES-256 密钥装进去"（或反过来），对称 blob 用 **AAD = `"pqchsm/symkey/v1"`**
封装，种子 blob 的 AAD 仍为空。AAD 不同 → GCM 校验失败，两类 blob 互不可用。

### 3.5 已知、本轮不解决的

- **任何 `LOGIN_USER` 进程都能直接调 TA**（不经 daemon）。它拿不到明文，
  但能往槽 0–6 装一把**自己 blob 里**的密钥 —— 与种子路径同一类问题，
  归 `FINAL-PLAN §3.2` 的按身份授权，不在本轮。
- **KEK 的根仍是可推导的 development HUK**（`BATCH2-REPORT §6`）：
  对能读 DNA、又能拿到 blob 的人，blob 不是真密文。这条不因本轮改变。
- **D7 的产品定位仍待定**：本轮只把"密钥装载"归到 TEE。
  bulk SM4 吞吐（数据面加速通路）的翻案条件见 `FINAL-PLAN §2 D7`，不动。
