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

---

## 4. 结果：改了什么

| 层 | 文件 | 改动 |
|---|---|---|
| PL | `hardware/rtl/bus/key_vault_axi.v` | 装载口门控（`wr_prot[1]==0`，与 `SECURE_ONLY` 无关）；`0x34 LOAD_REJ`；新输入 `ext_zeroize` |
| PL | `hardware/rtl/bus/sym_axi.v`、`sym_vault_top.v` | `CTRL.ZEROIZE` → `kv_zeroize` → 金库 `ext_zeroize` |
| PL | `hardware/rtl/board/zu3eg_hsm_top.v` | 金丝雀实例 `ext_zeroize` 接 0 |
| EL3 | `boot/atf/patch_atf_secmmio.py` | 槽 1 整槽排除（`pl_rd_ok/pl_wr_ok[1]=0`）；新 SiP `0x8200ff16 PQC_SYMKEY`（只认安全世界、每次带槽号、无锁槽、`WORD` 满 8 字拒、`COMMIT` 后核 VALID——判据折进 x0，因为 PTA 的 `thread_smc` 只带回 x0） |
| S-EL1 | `boot/optee/patch_optee_seedpta.py` | 第二条命令 `PTA_PQCHSM_SEED_CMD_SYMKEY`；参数高位必须为 0 |
| TA | `tee/ta/pqchsm_ta.c`、`tee/include/pqchsm_ta_proto.h` | `CMD_SYMKEY_NEW/LOAD/KAT`（12/13/14）；`GET_INFO` 特性位 bit4；失败当场 `ERASE` 该槽 |
| TA | 同上 | **顺带修了一个真 bug**：`seed_push` 把 `TEE_TASessionHandle`（指针宽）存进 `uint32_t`，aarch64 上 `TEE_OpenTASession` 往 4 字节变量写 8 字节、越界盖栈。能跑只因句柄恰好是小整数 |
| 普通世界 | `service/wire.h`、`libsdfe.c`、`sdfe.h` | `OP_SYMKEY_GEN/LOAD/KAT`（15/16/17）；`SDFE_GenerateKey/LoadKey/LoadKatKey`；`SDR_NOTSUPPORT`；`SDFE_ImportKey` 本地即拒、不发明文上线 |
| 普通世界 | `service/pqchsm_fpgad.c` | 删 `V_SLOT/V_KEYIN/V_SLOTC`；`OP_IMPORT_KEY` 拒并抹掉载荷；`session_end` 擦完自证 `KV_VALID` 落下；**两处过时注释改掉**（文件头"dk/sk 进 PL 的槽、对称密钥进 key_vault"，`session_end`"对称金库不在这里擦"） |
| 演示 | `service/sdf_demo.c` | [5]：明文导入被拒 ✅、KAT 槽 7 对 GB/T 32907 ✅、生成→blob→重装进另一槽同密文 ✅、blob 进不了槽 7 ✅；[7]：8 格模式向量改用 KAT 槽 |
| 板上工具 | `board/src/hsm_hwtest.c` | AES/SM4 KAT 与密钥边界扫描改记 **SKIP（不计入通过）**；EL3 拒槽 1 记为正面判据；金丝雀对照组从槽 1 换成槽 3 |
| 板上工具 | `service/wire_fuzz.py`、`board/scripts/pay_syzero.sh` | D7 的畸形输入用例；A/B 脚本里"KVOK 还是 1 是设计如此"那段注明已不成立 |
| 检查 | `tools/check_atf_patch.py`（新进 ctest：`atf_patch`）、`tools/check_ta_surface.py`、`tools/rtl_sim.sh` | 槽 1 排除 + `PQC_SYMKEY` 世界判据 + 空对照；TA 槽位纪律与 AAD；`key_vault_axi` 按 `SECURE_ONLY=0` 再跑一遍 |
| 文档 | `docs/REGISTERS*.md`、`docs/API*.md`、`service/README.md`、`tee-protocol`、`FINAL-PLAN`、`ARCHITECTURE-TARGET`、`STATELESS-PL-CONFLICTS`、`BATCH2-REPORT` | 寄存器表、命令表、状态注记 |

## 5. 主机侧验证

| 项 | 结果 |
|---|---|
| `test_key_vault`（`SECURE_ONLY=1` / `=0` 各一遍） | 9/9、9/9 |
| `test_sym_vault` | 6/6（新增"sym ZEROIZE 连金库一起擦"） |
| 空对照（RTL） | 去掉装载门 → `SECURE_ONLY=0` 那遍 1 条失败；断开擦除线 → `test_sym_vault` 1 条失败 |
| `check_atf_patch` | 通过；D7 空对照（槽 1 放回白名单 / 去掉世界判据）都被抓到 |
| `check_ta_surface` | 通过；去掉 KAT 槽判据 → 被抓到 |
| `tee_native`（含新的两条 blob 域分隔用例） | ALL PASS |
| TA / PTA / daemon / hsm_hwtest 的 C | 用替身头 `-Wall -Wextra` 编过（**只证明编得过**；真 TA 要构建机上的 OP-TEE dev kit） |
| 主机 `ctest`（除 `rtl_sim`，含新进的 `atf_patch`） | **56/56** |
| `tools/rtl_sim_fast.sh`（Verilator）：`key_vault` / `key_vault_axi`（两种形态）/ `sym_vault_top` / `axi4lite_firewall` | 4/4、9/9、9/9、6/6、6/6 |
| 同上：`axi4lite_xbar` | 7/8 —— `test_decode_viol_saturates` 失败。**与 D7 无关**：xbar 未改，`main` 上用 Verilator 同样失败，Icarus 下（合并回归）通过，属既有的 Verilator 专属问题 |

> 全套 Icarus `rtl_sim` 本轮**没有**在本分支上跑完（按仓库的两档分工，留给合入 `main` 前的关卡）。
> 改动涉及的三个顶层已用 Icarus 单跑过（见上表前几行），X 传播那一类问题已覆盖到这几个模块。

## 6. 上板清单（板子回来后按顺序做）

> **一键版**：演示形态下 `sh board/scripts/pay_d7.sh`（覆盖下面第 3、4、6、7 条：
> `ta_symkey` 正反两面 → `d7probe` 装载门反证 → `sdf_demo` → 会话后金库为空），
> 送检形态那一半走 `pay_seckat.sh`（第 5 条），第 8 条 `wire_fuzz.py` 从 Mac 跑。
> 需要在构建机上多编三样：`tee/host/ta_symkey`、`board/src/d7probe`、新的 `sdf_demo`。

> 板子这一轮不在，下面**一条都没有验过**。全部不涉及任何一次性/不可逆写入。

1. **构建**：位流（dev + sec 两份，看 `key_vault_axi`/`sym_axi` 有无新的 critical warning、WNS/WHS）；
   BL31（`patch_atf_secmmio.py`）；optee_os（`patch_optee_seedpta.py`）；TA；daemon / libsdfe / sdf_demo / hsm_hwtest。
   **PTA 与 TA 在主机上只用替身头编过**——首次真编译若报错，先看 `thread_smc` 与
   `TEE_TASessionHandle` 两处。
2. **装新 BOOT.BIN 前挂 0001**（有风险的启动实验规矩），golden 槽 7 保持不动。
3. **GET_INFO** 特性位 = `0x1C`（旧 TA 是 `0x0C`——据此判断 TA 是否换上）。
4. **sdf_demo 全部 ✅**（本地与 `demo/remote/run.sh --smoke` 各一遍），重点看 [5] 的四条新判据与 [7] 八格。
5. **反证（送检位流 + D7 BL31）**：`hsm_hwtest`（经 EL3）应出现"密钥仓（槽 1）：EL3 通用读写都拒"PASS，
   且 SKIP 计数 = 2（AES/SM4 KAT、密钥边界扫描）；`hsm_secneg` 照旧 rc=0。
6. **反证（演示位流）**：普通世界经 `/dev/mem` 写 `0x8001_000C/10/14`（**只在 dev 位流**——它对 NS 是
   RAZ/WI，不会 SError），读 `0x8001_0034 LOAD_REJ` 应按次数增长、`VALID_MAP` 不变。
7. **段末即擦**：sdf_demo 跑完后经 EL3 读 `SY_STATUS`，`KVOK=0`；daemon 日志里**不应**出现
   "SY_CTRL.ZEROIZE 之后 KV_VALID 仍为 1"。`pay_syzero.sh` 的 A/B 判据（低三位）复跑一遍。
8. **wire_fuzz.py**：新增 9 条 D7 用例全部被拒、服务不卡。
9. **吞吐**：`SYM_CRYPT` 64 KB 与改动前对比（分组路径未改，应持平）；单次 `SYMKEY_LOAD` 耗时记一笔
   （预计 11 次 PTA→SMC，亚毫秒级）。
10. **密钥边界扫描（板上版）**：sdf_demo [5] 装完槽 7 之后、会话结束之前，经 EL3 扫 `sym_axi`
    整窗口，确认 KAT 密钥的 4 个字一个都不出现（`key_vault` 窗口现在 EL3 读都拒，扫描对象只剩槽 2）。
