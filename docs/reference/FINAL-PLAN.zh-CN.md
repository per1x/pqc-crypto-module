# PQC 密码机 —— 最终方案（唯一权威来源）

> **本文是执行落地的唯一权威来源。** 与其它文档冲突时以本文为准。
> 论证过程与逐项证据在 [ARCHITECTURE-TARGET.md](ARCHITECTURE-TARGET.md)（架构与威胁模型）、
> [STATELESS-PL-CONFLICTS.zh-CN.md](STATELESS-PL-CONFLICTS.zh-CN.md)（冲突扫描全文）、
> [HSM-COMPARISON.md](HSM-COMPARISON.md)（业界对比与量产路径）——那三份是**支撑材料**，不是指令。
>
> **前提假设：量产形态有真信任根**——烧 `PPK0_HASH + RSA_EN`（认证根）+ `FUSE_AES` 或 PUF 注册
> （秘密根），CSU AUTH 位置起，HUK 走 CSU AES-GCM + device key 的真实路径。
> **这块 AXU3EGB 只是原型载体，它没烧 eFUSE 是原型阶段的流程选择，不是架构能力边界。**
>
> 执行会话请直接看 **§7 冲突登记表**（改什么）与 **§8 批 1 执行清单**（怎么做、怎么验）。

---

## 1. 最终架构决定

**TEE 主线**：长期机密（密钥种子及其派生根）由 **PS 的 OP-TEE 权威保管**；**PL 只做 PQC 计算**，
不保管任何跨操作的秘密；每次运算由安全世界把种子送进 PL 展开、**算完由硬件无条件擦除**。

**对称密码整体在 TEE 软件里做，PL 不参与**（见 D7）。

### PL 最终只剩这些

| 槽 | 内容 | 性质 |
|---|---|---|
| 0 | TRNG | 熵源，**不是 PQC 加速**——留它是为 SP 800-90B 可认证性 |
| 3 | ML-KEM 核 | PQC 计算 |
| 6 | ML-DSA 核 | PQC 计算 |
| 4 | 金丝雀 | 边界证明，恒被拒 |
| 5 | fan_ctrl | 非密码，自治散热，兼边界演示判别位 |
| — | xbar + AXI 防火墙 | **边界机制本身** |
| ~~1~~ ~~2~~ | ~~key_vault / sym_axi~~ | **移出**（对称进 TEE） |

⚠️ **PL 不是"不可信加速器"**：签名/解封期间展开后的明文私钥就在 PL 里，所以 PL 仍在安全边界**之内**，
只是不再是密钥的*保管者*。这正是擦除机与防火墙不可省的原因。

### 归属表

| 东西 | 放哪 | 一句话理由 |
|---|---|---|
| 密钥种子 `d‖z` / `ξ` | **PS-TEE** | 种子完全决定私钥，与私钥同级 |
| 完整私钥 | **不持久化** | PL 按操作展开、算完即弃 |
| ML-KEM / ML-DSA 运算 | **PL** | 计算密度高、PS 无等价指令 |
| **对称（AES/SM4/SM3）** | **PS-TEE 软件** | PL 在这里是减速器，见 D7 |
| TRNG | **PL** | 可表征、可出 SP 800-90B 证据 |
| KDR / KEK / pin_key / RMK-BEK | **PS-TEE** | 派生边界正好落在既有 `wrap.h` |
| keystore / 备份密文 | **普通世界文件** | 内容已由 TEE 包裹 |
| 种子通路守门 | **EL3 / BL31** | 唯一能区分调用者世界的层 |
| 槽位 FSM / 角色 / PIN 策略 | **PS-TEE**（状态）+ 普通世界（前端） | 秘密进 TEE，策略可留外 |
| 审计链 | **普通世界文件** | 签名私钥进 TEE，链本身留外（见 D10） |
| 备份仪式（RMK/Shamir） | **PS-TEE** | RMK 现在在普通世界生成，是个洞 |
| SDF / PKCS#11 前端 | **普通世界** | 不碰密钥值 |
| 防回滚锚 | **RPMB 写计数器** | 计数器推得上、拉不下 |

---

## 2. 决策点

格式：**业界怎么做 → 我们的处境 → 定。**

### D1 · 密钥保管：专用 SE vs FPGA + TEE
业界（Luna / YubiHSM / Marvell / 国密三级机）用认证过的防篡改安全处理器。我们这块 MPSoC 无专用 SE。
**定**：**PL 边界 + PS-TEE**——PL 提供物理隔离的运算通路，TEE 提供机密的权威保管。

### D2 · 种子每次重展开 vs 密钥常驻
业界无此取舍（密钥不出芯片）。我们保管边界在 TEE、PL 是外部计算单元，才有这个问题。
**定**：**每次运算从种子重展开，不设会话级缓存。** 擦除由**硬件在 S_FIN/DONE 无条件触发**，
不由任何软件（包括 TA）决定；展开区不带 valid 位、不带跨请求可引用的槽号语义。
> ⚠️ 早期版本曾允许"TA 门控的 PL 会话内缓存"，**现予撤销**：TA 崩溃/被拒服务/掉电时清除动作
> 根本发不出去，"TA 门控"只改变谁来清，不改变秘密在 PL 过夜的事实。

### D3 · 种子归属与通路守门（**主线成立的单点前提**）
现状 `/dev/secmmio` 的 SiP **不认调用者**，同样服务普通世界 root；种子今天经普通世界 daemon 栈（CODE-1）。

**定（口径已拧紧，不再有第二种解释）**：
- **批 1：种子收进 EL3/安全世界。** 由 BL31 生成并暂存种子，经**仅安全调用者**的 SiP 送入 PL 的种子暂存口。
  **明确不做"PL 内种子自生"**——那会让 PL 重新持有秘密生成能力，与"PL 无状态"相悖，且批 2 还得推翻。
- **批 2：种子保管上移 TA**，EL3 退回纯通路守门。
- 两批共用同一个 PL 侧种子暂存口（只认 `AxPROT[1]=0`、无读回路径、用一次即作废），**接口不变，只换保管者**。

⚠️ 两条硬约束：① BL31 必须按 `SCR_EL3.NS` 区分调用者世界，并把种子/装载偏移**从通用 `PL_WR` 白名单排除**
（两边缺一等于没做）；② TA 接管 PL 后，**普通世界 daemon 必须退到 TA 后面**——若保留平行驱动路径，安全模型直接破。

### D4 · PL 侧 wrapper
"PL 只做计算" **不等于**改用 `pqc_accel_axi`——已核实它连 `awprot`/`arprot` 端口都没有。
**定**：继续走带 `axi4lite_firewall` 的 SECURE_ONLY wrapper；若启用流式加速器，先补同一道防火墙。

### D5 · 硬件信任根
**定**：**量产按"有信任根"设计与宣称**，架构、代码、文档一律按终局写。原型板的形态差异按**标注**处理，不按缺陷处理。
⚠️ 两条写进产线自检：① **认证根与秘密派生根是两套独立烧写项**；② 供给后必须验 **CSU AUTH 位确实置起**
——位为 0 时上游 `zynqmp_huk.c` 会**静默降级**成 development HUK，失败形态是"看起来跑通了"。

### D6 · 物理防篡改 与 认证资质
**定**：**产品化前置，不进原型范围。** 防篡改属"需新增 BOM + 机械 + 台架验证"，是三项缺口里**未知数最少**的
（RTL 的 tamper 输入与金库清零通路已在）；资质**下游于**前两项，且素材侧已有实体，不是零起点。

### D7 · 对称密码：PL 还是 TEE　**【非批 1 阻塞项 · 产品定位待定】**
**定（方案 A）**：**对称整体进 TEE 软件，PL 不参与**；`key_vault` / `sym_axi` 移出交付边界。

**主理由是架构惯例，不是性能**——业界共识明确：**HSM 不做大批量对称加密，这是反模式**。
标准做法是**信封加密**：HSM 持主包裹密钥，数据密钥由 HSM 生成并包裹，**bulk 在数据面/软件里做**。
网络型 HSM 尤其如此（数据要送过去，大对象比本地算慢得多）。真正做硬件 bulk 对称的是数据面设备
（SmartNIC、QAT、内联加密器），不是 HSM。

次要理由：当前 PL 对称路径每分组约 10 次寄存器访问 ≈ **4 MB/s**，而 A53 带 ARMv8 Crypto Extensions
实测 AES-256-CBC **~1.14 GB/s**；且 **PL 里根本没有链式模式**（只有单分组变换），产品要的 AES-256-GCM
它做不了。顺带是安全升级（`aes_core.v` 自陈无 DPA 掩码，Crypto Extensions 的 DPA 抗性更高）。

> ⚠️ **一处过度概括的更正**："PL 做对称永远不划算"是**错的**。准确说法是"**用当前的迭代核
> （11 拍/分组 @75 MHz，天花板 109 MB/s）+ 寄存器逐分组路径**做对称不划算"。全流水 AES 在
> Zynq UltraScale+ 上可达 199 Gbps —— FPGA 本身不是瓶颈，**当前的核与路径才是**。

**翻案条件（明确写下，便于将来重新拍板）**：若产品定位要进国内服务器密码机市场，
**bulk SM4 吞吐（Mbps/Gbps 量级）常是规格书必答项**（与西方 HSM 报 ops/s 的惯例不同）。
此条一旦成立，D7 翻案——但**做法不是保留现在的对称金库**，而是另立一条**独立的数据面加速通路**：
全流水核 + DMA 主口 + 独立的 DDR 保护论证，与 PQC 密钥托管路径**分开设计、分开定级**。

代价必须一并算清（三道坎，第三道是决定性的）：① 核要从迭代式重做成全流水并做时序收敛；
② **PL 里目前零 DMA 主口**，且 PL→DDR 只能走 HP0（AFI 位宽由厂家 `psu_init` 定，HPC0/1 未配，
选它要改 BOOT.BIN）；③ **PL 加 DMA 主口 = PL 能读写任意 DDR**，绕过现有 AXI4-Lite 从机口防火墙，
必须另建 XMPU/SMMU 约束，而 XMPU 只毒化不 gate。**这不是加个 IP，是把边界重新论证一遍。**

**本条不阻塞批 1**：批 1 不动 `sym_axi` / `key_vault` 的功能，只补 `SY_CTRL` 擦除（见 §8 第 2 项）。

### D8 · 算法广度
**定**：**不补 RSA/ECC**，保持 PQC 专用定位。国密 SM2/SM3/SM4 若立项则在 TEE 软件里做（上游一个都没有）。

### D9 · CODE-1 要不要等 OP-TEE
**定**：**不等。** 批 1 用纯 RTL/BL31 手段先关掉，OP-TEE 打通后再把编排上移 TA。两者不互斥。

### D10 · 审计日志
**定**：**批 1 就接上**（现在 `tok->audit` 恒 NULL、事件静默丢弃）。
**审计链留在普通世界，只把签名锚的私钥移进 TA**——每次 append 两次 fsync，搬进 TA 要经 tee-supplicant
RPC 回普通世界写文件，性能和信任模型都变差。

### D11 · I/O 打包（**性能宣称的前提**）
实测数字按字节数对了一遍(`docs/TESTING.zh-CN.md:187-189`)：

| | 512 | 768 | 1024 | 比值 |
|---|---|---|---|---|
| KeyGen 实测 | 1.08 | 1.65 | 2.27 ms | 1 : 1.53 : **2.10** |
| 输出 ek 字节 | 800 | 1184 | 1568 | 1 : 1.48 : **1.96** |

**时间随传输字节数线性走，而不随计算量走**（ML-KEM 算力需求按 k² 涨，应是 1 : 2.25 : 4）。
**定**：当前交付路径是 **I/O 受限**。批 1 加 `IN_DATA`/`OUT_DATA` 的**4 字节打包**模式（写通路本来就有 `wr_strb`），
**然后重测**。在那之前，**性能相关的宣称一律不写死**——现在这组数字量的是管子，不是核。

### D12 · 上游 PKCS#11 TA：用还是不用
**定**：**不 fork，改为互补。** 见 §3。

---

## 3. 复用策略

### 3.1 直接拿

| 项目 | 许可 | 顶掉我们什么 |
|---|---|---|
| `optee_client`（libteec + tee-supplicant） | BSD-2 | 0 行（本来就没打算自己写）；但 tee-supplicant 是 REE-FS 的前提，要算进开机就绪流程 |
| **mlkem-native / mldsa-native** | Apache-2.0 / ISC / MIT | ✅ **已在 `tee/ta/vendor/`（16k 行）**。还可进一步顶掉 `pqc_liboqs.c`(266) + `oqs_rng.c`(185) **+ 整个 liboqs 依赖** |
| OP-TEE Secure Storage **REE-FS**（第一阶段刻意 `CFG_RPMB_FS=n`） | BSD-2 | 第一阶段 0 行；避开 ZynqMP 那条 `RPMB_FS→HUK→CSU_AES→CSU_PUF` 连锁 |
| `linaro-swg/optee_examples` 的 hello_world | BSD-2 | 0 行，但**正是批 2 那道门槛验证件**（注意：`OP-TEE/optee_examples` 是 404） |
| OASIS PKCS#11 v3.2 官方头 | OASIS | 已在仓内 |
| OpenSSL 3.5 LTS / SoftHSMv2 / pkcs11-tool / kryoptic | — | 0 行产品代码，用作 KAT 与差分测试口径 |

### 3.2 改造后拿（拿设计不拿代码）

| 项目 | 取什么 | 成本 |
|---|---|---|
| 上游 `ta/pkcs11` 的 **PIN 模型** | 加盐哈希 + 失败计数 + `COUNT_LOW`/`FINAL_TRY`/`LOCKED` 标志位形状（社区审计过，近 12 个月 14 个提交全是安全加固） | 对齐口径 200–400 行改动 |
| `CFG_PKCS11_TA_AUTH_TEE_IDENTITY` 的**思路** | 用 TEE Client Identity 代替 PIN 登录——**正是密码机"无人值守、开机没人输 PIN"要的东西**，标准 PKCS#11 没有 | 新增 150–250 行 |
| `core/drivers/crypto` 框架（versal / asu_driver 为样板） | 把 PL 核挂进 OP-TEE 的**唯一标准挂载点** | **全新** 800–1500 行 |
| `optee-pqc` / PQ-fTPM 的**构建配方** | 外部静态库进 TA 的坑：`libdirs/libnames`、`getauxval_stub`、`TA_STACK_SIZE`、`TA_FLAGS` | 几十行 |

⚠️ **两条明确警告**：`srinath1076/optee-pqc` 的 `oqs_align.c` 用 weak alias 覆盖全局 `free()`，
会劫持 libutee 自己的 free，**照抄会炸**；`p11-kit` 是 **LGPL-2.1**，闭源交付必须动态链接。

### 3.3 必须自己写

| 模块 | 规模 | 为什么上游覆盖不到 |
|---|---|---|
| `service/`（SDF/GM-T 0018 面 + daemon + PQCS + mTLS） | 3657 行 | 整个 GitHub 上 SDF 只有一个 0 star 的模拟 SDK。已过硅，最不该动 |
| `src/p11/` 的 PQC 机制与 v3.2 接口 | 2642 行中至少一半 | **上游是 v2.40、零 PQC**（见下） |
| `src/slot/` 的硬件与诚实语义 | 1918 中约 800–1000 行 | `SEED_STORAGE`、`hw_resident`、fail-closed 落盘钩子、强制清零、元数据 KMAC |
| `src/backup/` | 900 行 | PKCS#11 无 M-of-N/RMK-BEK/KEM 注入概念。**唯一必须整体搬进 TA 的**（RMK 现在在普通世界生成 = 洞） |
| `src/audit/` | 893 行 | 无对应概念。链留普通世界，签名钥进 TA |
| `src/hal/` accel 契约 + PL TRNG 驱动 | 2986 行 | 零上游件；`pqc_backend_t` 的**按句柄操作**（私钥根本不进软件）是任何上游对象模型都表达不了的 |
| **TA → PL 通路** | **全新** 800–1500 行 | 目标架构的这条路径**目前一行都不存在** |

### 3.4 为什么不 fork 上游 PKCS#11 TA（硬证据）

上游 `ta/pkcs11` 是一个很好的 RSA/EC/AES 软 HSM，但它**恰好在我们唯一真正需要的那层——PQC 机制层——是空的**，
而它能覆盖的那层（会话/PIN/对象/存储）**恰好是我们改造成本最低、且已经跑通的部分**。

- `token_capabilities.c` 里 `ML_KEM`/`ML_DSA` 命中 **0**；optee_os 4.10.0 全仓 grep 同样为 0
- `libckteec` 停在 **v2.40-errata01**，只有 `C_GetFunctionList`，没有 3.2 的 `C_EncapsulateKey`/`C_DecapsulateKey`
- 上游 issue #7462 追 3.2 至今 open；#7148 被维护者当天关闭，理由是 **mbedTLS 与 tomcrypt 都不支持 PQC**
  ——而 Mbed TLS 4.0 / TF-PSA-Crypto 1.0 正式版**零 PQC**，ML-DSA 排 2026 CQ2、ML-KEM 排 "Future"
- 上游持久对象是安全存储里的字节 blob，**表达不了 `hw_resident`**（私钥在 PL、软件只有槽号、PL 一重配即失效）
- `CFG_PKCS11_TA_TOKEN_COUNT` 默认 3 且**编译期定死**
- `plat-zynqmp/conf.mk` 里**根本没有 `CFG_PKCS11_TA`**

**算账**：真要 fork ≈ **3800 行改在别人的树上**，换掉我们 **5364 行**（slot 1918 + store 804 + p11 2642）
已过硅的自有代码，并从此背上长期 fork（上游 4 个月一版，每 tag 都要 rebase）。**不划算。**

**正确分工是互补**：我们的 `pqchsm_ta.c` 现在是无状态的"算+包裹"TA（无槽位、无 PIN、无会话、无持久存储），
而上游那个恰好补的就是这半。**但不要引入第二个 TA**——两个 TA 就是两套 PIN、两套对象、两套存储。
**把状态收进我们自己的 TA。**

### 3.5 PL 侧的开源件：不建议换

- **ML-DSA-OSH**（KU Leuven，MIT）：完整 FIPS 204、三等级×三操作运行时可切，54,942 LUT / 29 BRAM（Artix-7 数），
  ZU3EG 的 ~70.6k LUT 勉强装得下。但它**没有标准总线封装**，要自写 AXI wrapper、重新综合验时序、掩码另算。
  **定：只在我们现有核有明确缺陷时才换——已过硅的东西不要因为"上游更漂亮"而换。**
- **Adams Bridge 2.0**（CHIPS Alliance，Apache-2.0）：唯一同时有 ML-KEM+ML-DSA 且带一阶 DOM 掩码的工业级开源核。
  但参数集写死 Level 5、带防护约 335k LUT（**ZU3EG 装不下**）、接口是 Caliptra 内部约定。**只借架构思路。**
  ⚠️ 采纳前必须先读 Saarinen 在 hardwear.io 2025 的 *"Why 'Adams Bridge' Leaks"*。

---

## 4. 落地顺序

### 批 1 — 不依赖 OP-TEE、不依赖板子即可开工（**逐条见 §8**）
擦除覆盖 · EL3 种子暂存与门禁 · I/O 打包 · 接上审计 · 软件三条收口 · TA 两颗地雷 · 去 liboqs · 文档口径

> ⚠️ 擦除机是**独立于模型变更也必须修**的：ML-DSA 三个核 `grep -c zeroize` = **0/0/0**，
> `ram_dp` 无复位口 → 今天每跑完一次签名，展开态私钥就原样留在 PL 里。

### 批 2 — 硬门槛：最小 TA 先在板上跑通并留串口证据，在那之前不动 `src/` 一行
① OP-TEE 真正启动（解决 BL32 入口）→ ② TA→PL 通路（`core/drivers/crypto` 挂载点，**同时把 daemon 退到 TA 后面**）
→ ③ KEK/wrap 上移 + KDR provider → ④ 槽位秘密与 pin_key 进 TA → ⑤ **备份仪式整体搬进 TA**（RMK 不再在普通世界生成）
→ ⑥ 对称服务改由 TEE 软件提供，PL 的 `key_vault`/`sym_axi` 移出边界 → ⑦ PQC 金库降级/删除

### 量产供给（排产工作，不是能力缺口）
烧写计划（认证根 + 秘密根**两套独立**）→ OP-TEE HUK 供给 + **产线自检 CSU AUTH 位** →
`pqc_kdr_provider_optee()` → 打开 `CFG_RPMB_FS`（注意 ZynqMP 那条 HUK/CSU_AES/CSU_PUF 连锁）→
产线密钥保管（Bootgen HSM 模式，PPK 私钥不落构建机）

### 产品化前置
物理防篡改（BOM + 机械 + 台架）→ 认证资质（下游于前两项）

---

## 5. 红线（不因本方案放宽）

⛔ **绝不烧 eFUSE、不动 BBRAM 锁存位、不重烧 RPMB 密钥、不做任何一次性/不可逆写入。**

准确定义（`docs/SECURITY.md` L35-36）：**不可逆动作需板主显式同意，默认答案是不**——可推翻的默认拒绝，
不是物理不可能。现行理由是"不可逆 + 只有一块板"，即**资源与流程约束，量产时自动失效**。

配套：不可逆动作 + 会撒谎的状态读取 = 不可接受（RPMB `provision` 保持编译期关闭）。

---

## 6. 对外宣称口径

**目标（量产）形态**：
> 一台**以 PQC 硬件核为差异化**的密码机：ML-KEM 与 ML-DSA 全参数集在 FPGA 专用数据通路上
> 逐字节对齐 ACVP、硅上验证；密钥种子由 PS 的 TEE 保管、私钥不出安全世界；
> 信任根锚在 eFUSE，安全启动闭合镜像替换。

**当前（原型）形态**——对外材料需带这一句：
> 本机为原型载体，**信任根尚未供给**，KDR 走设备 DNA 派生，提供运行时隔离 + 设备绑定级别的保护；
> 物理防篡改与合规资质属产品化前置。原型上的测量结果不作为量产安全性质的证据。

⚠️ **性能相关的话在 D11 的 I/O 打包完成并重测之前一律不说。** 现在能站住的是
"PQC 在专用硬件通路上跑通并逐字节对齐 ACVP"，**不是"更快"**。

---

## 7. 冲突登记表（以"PL 无状态 + 密钥归 TEE"为尺）

严重度：**硬冲突** = 设计直接相反 · **需重构** = 能改但要动结构 · **仅措辞** = 只是文档说法。
全文与证据见 [STATELESS-PL-CONFLICTS.zh-CN.md](STATELESS-PL-CONFLICTS.zh-CN.md)。

### A · PL 内金库与槽位 ABI —— 与模型最正面的对撞

| ID | 冲突点 | 与模型怎么打架 | 严重度 | 消解 | 批次 |
|---|---|---|---|---|---|
| V-01 | `mlkem_axi.v` **16 槽 × 4 KB dk 金库** | KeyGen 把完整 dk 写进 PL 槽、Decaps 按槽取用；**私钥的权威保管方成了 PL**，跨会话存活 | 硬冲突 | 先降级为"单次操作展开区"（清除条件改 S_FIN 无条件清），再决定是否连 ABI 删 | 批2 |
| V-02 | `mldsa_axi.v` **8 槽 × 8 KB sk 金库** + `S_LOAD`/`S_STORE` | 同上，sk（含 K、s1/s2、t0）按槽复用 | 硬冲突 | 同 V-01 | 批2 |
| V-03 | `key_vault.v` 8 槽对称金库 | 对称密钥权威保管方是 PL | 硬冲突 | 随 D7 方案 A 移出边界；**批 1 只补擦除，不动功能** | 批2 |
| V-04 | `dk_lock` / `sk_lock` 一次性闩 | 闩的目的是把"私钥留在 PL"升级成硬件性质——**守在与模型相反的方向** | 硬冲突 | 随 V-01/02 删；"关闭导出"的闸门改落 TEE 策略侧 | 批2 |
| V-05 | `MODE` 的 `DK_TO_SLOT`/`DK_FROM_SLOT`/`SK_TO_SLOT`/`SK_FROM_SLOT`/`SLOT[9:6]` | 寄存器面把"按槽引用私钥"编码成对外 ABI | 硬冲突 | MODE 只留 OP/PSET；daemon、libsdfe、board/src 宏同步 | 批2 |
| V-06 | `KEYSTAT` / `KEYPSET` | 对外暴露"PL 有状态且可查"；泄密钥存在性/数量/参数集 | 需重构 | 随金库删，只留与密钥无关的健康位 | 批2 |
| D-01 | `pqchsm_fpgad.c` 的 `keys[]`/`dsa_keys[]`（"句柄就是槽号"） | 密钥生命周期管理落在**普通世界单线程 daemon**，TEE 完全不出现 | 硬冲突 | 句柄表上移 TA，daemon 退化为纯搬运 | 批2 |
| D-02 | `-lock` 启动参数 | 交付形态的安全性质由普通世界进程启动时设置，且守错方向 | 硬冲突 | 随 V-04 删 | 批2 |

### B · 算完即弃在 RTL 层未实现 —— **批 1 优先，且独立于模型变更**

| ID | 冲突点 | 严重度 | 消解 | 批次 |
|---|---|---|---|---|
| Z-01 | ML-DSA 三核**无 zeroize 端口**（`mldsa_engine.v:59-63` 自陈是"已知缺口"） | 硬冲突 | engine 内**一台共享擦除机**广播三核 | **批1** |
| Z-02 | `sign.v` 的 `key_out`(=K)/`tr_out`/`mu`/`rhopp`/`ctilde` 只在 `!rst_n` 清 | 硬冲突 | 核复位改 `rst_n && !wipe`，一拍带走 | **批1** |
| Z-03 | ML-KEM 核内 BRAM（`u_bank`/`u_ekbuf`/`u_cbuf`）无擦除；`ram_dp` 无复位口 | 硬冲突 | 同一台擦除机广播 | **批1** |
| Z-04 | ML-KEM 三处 `sha3_core.zeroize` 接死 `1'b0`（与 `mldsa_axi` 的处理自相矛盾） | 需重构 | 三行改接真信号 | **批1** |
| Z-05 | 对称核 zeroize 有能力，但 daemon **无 `SY_CTRL` 偏移、从未发过** | 需重构 | daemon 补偏移 + `session_end` 发 ZEROIZE | **批1** |

### C · 软件密钥栈在普通世界

| ID | 冲突点 | 严重度 | 消解 | 批次 |
|---|---|---|---|---|
| PS-04 | DEV 下无 provider **自动回退到编译期常量桩** | 硬冲突 | 去掉自动回退，非 demo 目标必须显式安装，失败即拒启 | **批1** |
| PS-07 | `SLOT_POLICY_SEED_STORAGE` 把种子落盘进 keystore | 硬冲突 | 安全 profile 下**拒绝**（不是忽略） | **批1** |
| PS-11 | 种子经普通世界 daemon 栈（**CODE-1**） | 硬冲突 | **批 1 收进 EL3**（见 D3），批 2 上移 TA | **批1** |
| PS-12 | `secmmio` SiP **不区分调用者世界** | 硬冲突 | BL31 按 `SCR_EL3.NS` 分级 + 白名单排除种子/装载偏移 | **批1** |
| PS-25 | `pqc_random_bytes` 用 `hwrng_available()`（stub 也为真） | 需重构 | 改 `hwrng_is_hardware()`，取不到硬件熵即停机 | **批1** |
| PS-22 | TA `TEEC_LOGIN_PUBLIC` + `CMD_KDF_DERIVE` **接受任意 label** | 硬冲突 | ⚠️ 可用 `label="pqc-hsm/storage-kek"` + keystore 头部明文 salt **把存储 KEK 原样要出来**；**有真 HUK 后泄的就是真 KEK**。删命令 + 换 login type | **批1** |
| PS-24 | `CMD_KEK_SET`+`CMD_UNWRAP` 构成**通用解包谕言机** | 硬冲突 | 同上 | **批1** |
| PS-01 | `src/store/wrap.c` KEK 派生与 wrap/unwrap 全程普通世界 | 硬冲突 | 换 TA 调用；**边界正好落在 `wrap.h`**，`src/` 其余不动 | 批2 |
| PS-02 | `kdr.c` 的 `g_root[32]` 在普通世界内存 | 硬冲突 | 新增 `pqc_kdr_provider_optee()`，纯转接层 | 批2 |
| PS-06 | `slot_internal.h` 的 `pin_key`/`sk`/`seed` 跨操作常驻普通世界 | 硬冲突 | slot_t 只留句柄与策略；三项进 TA，PIN 验证子计算随 pin_key 进 TA | 批2 |
| PS-13 | PKCS#11 模块**在调用方进程内静态链接 slot 层** | 硬冲突 | 改瘦客户端（工程量最大，推到批 2 之后） | 批2 |
| PS-16/17 | 备份 RMK/BEK/Shamir、注入链的 ss/cek/seed 在普通世界栈 | 硬冲突 | **备份仪式整体搬进 TA**（RMK 不再在普通世界生成） | 批2 |
| PS-18 | 审计锚的 ML-DSA 身份私钥在普通世界 | 需重构 | **只把签名私钥移进 TA**，审计链留普通世界（见 D10） | 批2 |
| PS-19 | RPMB 认证密钥是 SD 上 0600 文件 | 需重构 | **不动**（撞红线）；量产走 OP-TEE RPMB-FS | 量产 |
| PS-20 | mTLS 设备私钥是 SD 上 0600 文件 | 需重构 | 搬进 TA（可选，签名在 S-EL1） | 批2 |
| PS-03 | KDR 的 device-dna 从普通世界读 DNA | 硬冲突 | **由量产信任根解决**：接从真 HUK 取根的 provider | 量产 |

### D · PL 缓存驻留张力（本方案自身的自相矛盾，已消解）

| ID | 位置 | 问题 | 处置 | 批次 |
|---|---|---|---|---|
| PLAN-01 | 本文 D2 | 早期版本"允许 TA 门控的 PL 会话内缓存"与"PL 不持有秘密"**直接矛盾**，且使 D2 自身立论失效 | **已撤销**：改为单次操作、硬件在 S_FIN 无条件擦 | **批1**（文档） |
| PLAN-05/06 | `ARCHITECTURE-TARGET.md` §2 表、§7.3 | 同源表述 | 三处**同时**降级，不能一处删一处留 | **批1**（文档） |

### E · 叙事口径 —— 依赖"私钥受 PL 保护"的表述要改主语

约 20 处（`README`(中英)、`docs/API.md` 的 "Where private keys live" 整节、`docs/ARCHITECTURE.md`
"Key vault and firewall"、`docs/SECURITY.md` 边界节与限制表、`docs/REGISTERS.md` 金库寄存器表、
`security-policy.md`）。**"不出芯片"的主语要从 PL 变成 TEE。** 多数为**仅措辞**，但两处是**证明方法失效**：

- `README` 证据表的"密钥仓反证" + `board/demo/run_demo.sh` 第③节——那个演示是**扫 PL 地址空间证明密钥在 PL 但读不到**。
  **处置：不要删，改成"加一段"**——运算结束后再扫一次 + 断言 VALID 已清。旧扫描仍是"无总线读路径"的结构性回归。
- `docs/API.md:248-260` 整节要重写。

**批 1 的文档纪律**：在 `docs/SECURITY.md`、`security-policy.md` 等**送检级材料**上，
**只在文首加一句形态标注、正文一字不动**——实现未落地就改写这些，等于承诺未交付能力。
待 EL3 种子暂存 + 门禁 + 擦除覆盖三件事在板上跑通后，再一次性逐字改写。

---

## 8. 批 1 执行清单（交给执行会话）

**入选标准**：不依赖 OP-TEE、不依赖开发板即可开工。RTL 项可在 cocotb 里仿真验证，
"上板 pending" 一列标出哪些必须留到有板子时才能确证。

**可用的验证设施（已核实）**：`ctest` 41 个目标；`ctest -R rtl_sim` 经 `tools/rtl_sim.sh` 跑全套 cocotb
（需 `iverilog` + `.venv-rtl`，缺环境会 SKIP 而非失败）；静态检查 `check_zeroize.py`、`check_no_readback.py`、
`ct_audit.py`、`tools/check_profile.sh`。

| # | 事项 | 改哪里 | 验证方式 | 上板 pending |
|---|---|---|---|---|
| **1** | **ML-DSA 擦除机**（Z-01/Z-02） | `hardware/rtl/mldsa/mldsa_engine.v`（加一台共享擦除机 + `wipe_en` 广播）、`sign.v`/`keygen.v`/`verify.v`（加 zeroize 端口；每块 `ram_dp` 的 B 口在 wiping 期强制 `addr=cnt,we=1,din=0`；核复位改 `rst_n && !wipe`） | `ctest -R rtl_sim`（`test_mldsa_engine` / `test_mldsa_sign` / `test_mldsa_keygen` / `test_mldsa_verify`）；**新增断言**：跑一次 Sign → ZEROIZE → 经 dbg 口逐地址扫，断言 sk 派生量一字节不剩 | 实际擦除拍数与 daemon 轮询上限 |
| **2** | **ML-KEM 擦除 + sha3 接线**（Z-03/Z-04） | `hardware/rtl/mlkem/keygen.v`/`encaps.v`/`decaps.v`（加 zeroize 端口与擦除机；三处 `.zeroize(1'b0)` 改接真信号）、`mlkem_axi.v`（`zeroize_all` 转下去，并把 `:340/348/358` 的 `rst_n && !zeroize_all` 改回纯 `rst_n`——有真擦法就不该再拿复位充数） | `ctest -R rtl_sim`（`test_mlkem_*`、`test_sha3_core`） | 同上 |
| **3** | **对称核 ZEROIZE 接上**（Z-05） | `service/pqchsm_fpgad.c`：补 `SY_CTRL`（`S_SYM+0x04`）偏移，在 `session_end()` 连同两个 PQC 金库一起发 ZEROIZE，按 `wait_not_wiping` 同款纪律等落 | 本机编译 + `ctest -R daemon_failclosed`；RTL 侧 `test_sym_vault` 已覆盖擦除语义 | **是**（需板子确证真发出去了） |
| **4** | **PL 种子暂存口**（配合 D3） | `hardware/rtl/bus/mlkem_axi.v` / `mldsa_axi.v`：新增种子暂存寄存器（**只认 `AxPROT[1]=0`、无读回路径、用一次即作废、START 当场清暂存与字计数**）；`MODE` 加"用暂存种子"位 | `ctest -R rtl_sim`：扩 `test_mlkem_axi`/`test_mldsa_axi`，**必须含负测试**——非安全事务写种子口被拒、读种子口恒 0 | 与 EL3 联调 |
| **5** | **EL3 种子生成与门禁**（PS-11/PS-12，**CODE-1 正解**） | `boot/atf/patch_atf_secmmio.py`：新增仅安全调用者的种子装载 SiP；`pl_permit()` 把种子/装载偏移**从通用 `PL_WR` 白名单排除** | `python3 -c "import ast;ast.parse(...)"` 语法 + 补丁幂等性 dry-run；**逻辑正确性只能上板验** | **是**（本项主体） |
| **6** | **I/O 4 字节打包**（D11） | `mlkem_axi.v` / `mldsa_axi.v` 的 `IN_DATA`/`OUT_DATA`（写通路本来就有 `wr_strb`）；daemon 对应改用打包写 | `ctest -R rtl_sim`（`test_mlkem_axi`/`test_mldsa_axi` 加打包路径用例，含"打包与逐字节结果逐字节一致"对拍） | **是**（**重测吞吐**，性能宣称的前提） |
| **7** | **接上审计日志**（D10） | `cli/pqchsmd.c` + `src/p11/p11_module.c`：调 `hsm_token_attach_audit`；定一个默认日志路径与开关 | `ctest -R "audit|anchor|e2e"` —— **纯 host 可验** ✅ | 否 |
| **8** | **软件三条收口**（PS-04/07/25） | `src/crypto/kdr.c`（删自动回退到桩）、`src/slot/slot.c`（安全 profile 拒绝 `SEED_STORAGE`）、`src/util/util.c`（判定改 `hwrng_is_hardware()`） | `ctest -R "kdr|slot|keystore|profile_no_stub_kdr|hwrng"` —— **纯 host 可验** ✅ | 否 |
| **9** | **TA 两颗地雷**（PS-22/24） | `tee/ta/pqchsm_ta.c`：`CMD_KDF_DERIVE` 改为只接受白名单 label（或整条删）；`CMD_KEK_SET`+`CMD_UNWRAP` 收紧；session 打开的 login type 从 `TEEC_LOGIN_PUBLIC` 换掉 | `tee/tests` 原生构建（x86 native，不需要板子）—— **纯 host 可验** ✅ | 否 |
| **10** | **去 liboqs**（复用收敛） | ⚠️ **先解决构建位置**：vendored 的 mlkem-native/mldsa-native 现在只在 `tee/ta/vendor/` 下、**只被 OP-TEE 的 `tee/ta/sub.mk`（`srcs-y +=`）引用，根 CMakeLists 完全够不到**。第一步是把它挪到共享位置（如 `third_party/`）并让两套构建都能用。然后 `src/crypto/pqc_liboqs.c`(266) 改接它；连带删 `src/crypto/oqs_rng.c`(185) 的全局 RNG 临界区；**并删掉 `CMakeLists.txt:38-39` 找不到 liboqs 就 `FATAL_ERROR` 那段**（去掉外部 brew 依赖才算真去成） | `ctest -R "pqc_roundtrip|pqc_meta|selftest|kat_parse|e2e"` —— **纯 host 可验** ✅ | 否 |
| **11** | **文档口径**（PLAN-01/05/06 + E 组） | 本文 D2 已改；`ARCHITECTURE-TARGET.md` §2/§7.3 三处**同时**降级；送检级材料（`docs/SECURITY.md`、`security-policy.md`）**只在文首加形态标注、正文不动** | `ctest -R shell_var_braces`（脚本类）；文档本身人工复核 | 否 |
| **12** | **REGISTERS 与 RTL 对齐**（DOC-3） | `docs/REGISTERS.md`(+zh)：16 槽（非 4）、`KEYSTAT` 布局、独立 `0x34 KEYPSET`、`MODE` 缺的三个字段；并统一三处互相矛盾的擦除周期数（8192/16384/65536） | 人工对照 RTL；建议加一条 CI 检查把寄存器表与 RTL localparam 对拍 | 否 |
| **13** | **消除两处跨世界重复实现**（防漂移） | ① **PWRP 包裹格式两份实现**：`tee/ta/ta_wrap.c`(277) 与 `src/store/wrap.c`(211) 是**同一个线格式**写了两遍。**不要求合并实现**（TA 侧用 `TEE_` API、host 侧用 OpenSSL，后端本就不同），要求**格式定义只有一处**：把 magic/版本/字段偏移/AAD 组装抽成一个共享头，两边都 include。② **KMAC256 两份**：`tee/ta/ta_kdf.c`(94，自带 `ta_fips202.c` 海绵) 与 `src/crypto/kdf.c`(105，OpenSSL EVP)——同样不合并（TA 里没有 OpenSSL，自实现是**正当的**），但必须加一组**跨实现 KAT 对拍**，钉死两边对同一 `(ikm,salt,label,len)` 输出逐字节一致 | 新增一个 host 侧对拍测试（把 TA 的 `ta_fips202`/`ta_kdf` 以原生方式编进测试目标，`tee/tests` 已有原生构建先例）；`ctest -R "wrap|kdf"` —— **纯 host 可验** ✅ | 否 |

**建议执行顺序**：先做 7/8/9/13（纯 host、可立即验、风险最低）→ 10（去 liboqs，**先挪 vendor 目录再改接口**）
→ 1/2（RTL，可仿真）→ 4/6（RTL + 需与 EL3 配套）→ 5（EL3，主体待上板）→ 3（daemon，待上板）→ 11/12（文档收尾）。

**上板前的既有纪律不变**：先证明能 JTAG 救援再上板；位流可运行时换、断电回黄金槽；
`plharness.sh` 那套（不在前台 SSH 里做、退出时无条件恢复网络）。

---

*本文只做决策记录；不改功能代码、不碰开发板、不做任何不可逆动作。*
