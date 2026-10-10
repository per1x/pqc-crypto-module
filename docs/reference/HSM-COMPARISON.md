# 本项目 vs 业界成熟 HSM / 密码机 —— 对等对比

> 目的：把本项目（`pqc-hsm-fpga`，ZU3EG MPSoC 原型）放进业界坐标里，诚实、对等地看差距与差异化。
> 方法：业界事实经 2026-08 联网核实，来源见文末；本项目事实以仓库代码/RTL 与
> [ARCHITECTURE-TARGET.md](ARCHITECTURE-TARGET.md)、[SECURITY.md](../SECURITY.md) 为准。
> 不吹不贬——凡没有公开数字处标"未公开/需实测"，绝不杜撰。

---

## 0. 归因纪律（读本文前先看这一节）

本文对每一处"本项目没有 X"，都必须归因到**最窄的真实主语**，分四类。混用这四类是本文档上一版最大的
毛病——把"本项目选择不做"写成了"这颗硅做不到"。

| 类 | 含义 | 主语该怎么写 | 本项目实例 |
|---|---|---|---|
| **(a)** | **硅具备，本项目选择不启用** | 写"本项目"，**不**写"这颗硅/这块板" | eFUSE 全套：`PPK0/1_HASH`、`RSA_EN`、`ENC_ONLY`、`FUSE_AES`、PUF 注册、`USER_0…7` 单调计数 |
| **(b)** | **这块 ALINX 板卡缺的硬件**（量产载板上就没了） | 写"this board / 这块 AXU3EGB" | `VCC_PSBATT` 无电池 → BBRAM 保不住密钥 |
| **(c)** | **需新增产品硬件** | 写"需增加 BOM/机械设计" | 防篡改网格、光电/电压毛刺传感器、外壳 |
| **(d)** | **成因未定位的实测失败** | 写"实验有缺口，未定论" | PUF 黑钥被 BootROM 拒收（见 §2.1 注） |

**红线的准确表述**：`SECURITY.md` L35-36 的原始定义是"不可逆动作需板主显式同意，**默认答案是不**"——
一条**可被板主推翻的默认拒绝**，不是物理不可能。`profile.h:14-15` 自己写的理由是"不可逆、**只有一块板**"
——这本身就是**资源与流程约束，量产时自动失效**。故本文一律不用"永久排除 / 做不成 / 补不了"，
改用"本项目在现行红线下不启用"。

---

## 1. 一句话结论（先给判断，细节在后）

本项目是一个**可信的"PQC 密码核在硬件里跑通"的工程原型**：ML-KEM 与 ML-DSA 全参数集在 PL 里逐字节
对齐 ACVP、跑在一道能证明拒绝普通世界读的 AXI 边界内——在 **native-PQC 跑在专用硬件数据通路**这个细分
角度上，比多数商用 HSM 的"固件加 PQC"更靠前一步。

但它现在**不是一台 HSM 产品**，缺的三样是信任根、物理防篡改、合规资质。**这三样都不是设计天花板，
但也都不是"量产自然就有"**——它们各自属于不同的类别、剩余工作量差了一个数量级：

> **上限是本项目的策略选择，不是硅的能力边界；但下限是一堆没写的代码和没建的流程，不是"烧一下就有"。**
> 这两句必须同框出现，缺任何一句都是失真。

---

## 2. 对标对象一览

| 类别 | 代表产品 | 形态 |
|---|---|---|
| 通用商用网络 HSM | **Thales Luna Network HSM 7/8**（含 Thales TCT T-Series） | 网络设备 / PCIe 卡 / 服务化 |
| 云 HSM 底层芯片 | **Marvell LiquidSecurity 2**（AWS CloudHSM / Azure 背后） | PCIe 适配卡 / DPU |
| 小型 / USB HSM | **YubiHSM 2 (FIPS)** | USB-A nano |
| 云托管 HSM | **AWS CloudHSM / Azure Managed HSM** | 云服务 |
| 国密密码机 | **三未信安 / 数盾 等服务器密码机** | 1U/2U 机架 / PCIe 卡 |
| 本项目 | **pqc-hsm-fpga** | ZU3EG MPSoC 单板原型 |

---

## 3. 逐维度对比

每一条缺口用同一个四段模板写：**现状 → 成因归类 → 今天攻击者能做什么（一个字不软）→ 补齐要什么**。

### 维度 1 — 硬件信任根 / 安全启动

| | 做法 | 强度 |
|---|---|---|
| Thales Luna 7/8 | 认证过的安全模块 + 安全固件更新 + tamper 响应 | FIPS 140-3 L3 边界内的信任根 |
| Marvell LS2 | OCTEON DPU 内安全启动 + 隔离分区 | 140-3 L3 + CC + eIDAS + PCI PTS |
| YubiHSM 2 | 专用安全芯片，密钥不出芯片 | 140-3 L3（证书 #5302, 2026-06） |
| 国密三级机 | 芯片级信任根 + 三层密钥体系 | GM/T 0028 三级 |
| **本项目** | **信任根未启用**（非"不存在"） | 见下四段 |

**现状**：认证通路是活的，但没有信任根锚。BOOT.BIN 不锚在 eFUSE；OP-TEE 侧 HUK 退化为 `SHA-256(Device DNA)`。

**成因归类**：**(a) 硅具备、本项目选择不启用。** 这颗 ZU3EG 完整具备 `PPK0/1_HASH`（SHA3-384 摘要）、
`RSA_EN`、`ENC_ONLY`、`FUSE_AES`、PUF 注册、`USER_0…7`——见 Xilinx 官方 XilSKey 示例头文件逐位注释。
本项目按红线不烧，故未启用。另有 **(b)**：这块 AXU3EGB 的 `VCC_PSBATT` 没接电池，BBRAM 保不住密钥
（量产载板上此项消失；官方甚至提供 `BBRAM_DISABLE` 位供量产永久关掉这条攻击面）。

**今天攻击者能做什么**（不软化）：拿到 SD 卡的人可以把 BOOT.BIN 换成自制镜像，板子照跑。注意一个易被
误解的机械细节：本项目记录过"零 eFUSE 的 RSA 认证启动可跑"，所以严格说不是"没有认证"，而是
**认证不锚在 eFUSE**——`bh_auth_enable` 按定义就是"跳过 eFUSE 里的 PPK 摘要校验"，因此它对"攻击者换镜像"
这个威胁提供的保护是**零，不是部分**。同理，`SHA-256(Device DNA)` 派生的 HUK 不是机密（DNA 从 EL1 经
SMC 就能读）。

**补齐要什么**：见 §4 的完整清单。**关键别踩的坑：烧 `PPK0_HASH + RSA_EN` 得到的是"启动镜像防替换的
认证根"，而 keystore/KDR 需要的是"秘密派生根"（`FUSE_AES` 或 PUF 注册 + 黑钥）——这是两件独立的事、
两套独立的烧写项与产线工序。**把二者混为一谈是本维度最常见的错误。

> **⚠️ (d) 类待澄清项 —— PUF 不是死路，是实验有缺口。** 本项目实测 PUF 黑钥被 BootROM 在触及 PUF 前
> 拒收（`PUF_CMD=0`、`KEY_RDY=0`），曾被记为定局。但三份材料合起来看出一个从未做过的组合实验：
> `build_puf_boot.sh` 文件头自己写着"变体 A（**不加 RSA**）"，其 bif 里没有 `bh_auth_enable`；
> 而 XAPP1333 明写片外黑钥与辅助数据在使用前**一律先过 RSA-4096 认证**。本项目又单独验证过零 eFUSE 的
> RSA 认证启动可跑——**这两件事从没合过**，成本是一次冷启动。
> 但**即便合上跑通，PUF 引导头模式也不足以让 provider 诚实地报 `hardware_backed=1`**：没有 eFUSE 锚定
> 认证时，攻击者把同一份黑钥/辅助数据放进自己的镜像、在同一块板上启动，CSU 照样把 PUF KEK 展开给他用。
> 他读不到密钥，但能用——与本项目 XMPU 那条"挡的是读不是用"是同一个失效模式。

### 维度 2 — 密钥保管形态

| | 保管在哪 |
|---|---|
| 商用产品 | 认证过的防篡改安全处理器/SE 内，密钥全生命周期不出边界 |
| **本项目（现状）** | 私钥在 **PL 金库 BRAM**（16 槽 ML-KEM / 8 槽 ML-DSA / 8 槽对称），无总线读路径 + 单向闩；但**种子过普通世界栈**（CODE-1） |
| **本项目（TEE 目标形态）** | 种子权威保管收进 PS-TEE，PL 退无状态、按操作重展开 |

**简析**：商用是"专用防篡改 SE 一手包办"；本项目是"PL 算 + PS 管"的分工，隔离靠 PL 内 AXI 防火墙
（RAZ/WI）而非认证 SE。这个分工在研究上有意思，但边界强度受维度 1 约束——防火墙挡得住运行时普通世界读，
挡不住能改镜像的人。

### 维度 3 — 物理防篡改

| | 机制 |
|---|---|
| Thales Luna | tamper 检测与响应、抗物理攻击 |
| Marvell LS2 | 认证边界内防篡改（PCI PTS 等） |
| YubiHSM 2 | 防篡改封装 + 防篡改审计日志 |
| 国密三级机 | 三级要求主动屏蔽层/传感器/触发清零 |
| **本项目** | 仅 SYSMON 过温（OT）一路接到金库清零，**从未物理触发过**（需 125 ℃）；无网格/光电/电压毛刺传感器 |

**成因归类**：**(c) 需新增产品硬件。** 与维度 1 性质不同——那是"硅里有但没启用"，这是"硅和板上都没有"。

**但别把它写成最难的一项——它其实是三项里未知数最少的**：RTL 里 tamper 输入与金库清零通路**已经存在**，
缺的是 BOM（网格/传感器）+ 机械设计 + 一次台架验证。这是采购与工程，不是研究。要区分两件事：
**tamper 响应**（工程可达）与**实验室认可的 tamper 证据**（要设计 + 送检）。

### 维度 4 — 认证资质

| | 资质 |
|---|---|
| Thales TCT T-Series | **FIPS 140-3 Level 3**（固件 7.15.1，2026-08），首个美产 PQC HSM 达此级，含 CNSA 2.0 全套 |
| Marvell LS2 | **FIPS 140-3 L3 + Common Criteria + eIDAS + PCI PTS** |
| YubiHSM 2 FIPS | **FIPS 140-3**（CMVP 证书 #5302，2026-06） |
| 国密三级机 | **GM/T 0028 安全三级 + GM/T 0030 + 商用密码产品认证** |
| **本项目** | **无任何资质**（ACVP 本地跑通 ≠ 认证） |

**成因归类**：第四种性质——**不在硅上也不在板上，是流程、时间、钱和第三方**，且**下游于**维度 1 和 3。
所以正确表述是**排序**（前置未满足，故排在后面），不是否定（"谈不上"）。

**已有的送检资产别当零起点**：`security-policy.md` 草稿、ACVP 逐字节对齐证据、自检清单、缺口表都在仓库里。
准确说法是"**前置未满足，材料侧已有实体**"。

### 维度 5 — 算法覆盖（**本项目相对领先的点**）

| | 传统 | 国密 | PQC（原生运算） |
|---|---|---|---|
| Thales Luna 7/8 | RSA/ECC 全谱 | — | **ML-KEM / ML-DSA / LMS**（2026 起在 140-3 内） |
| Marvell LS2 | RSA/ECC/AES 全谱 | — | 现场可升级，**尚非原生就绪** |
| YubiHSM 2 | RSA 2048-4096、ECC P-224…P-521/secp256k1/Ed25519 | — | **无原生 PQC** |
| AWS CloudHSM | RSA/ECC/AES/3DES/HMAC | — | **无原生**；仅作 opaque WRAP 键存储，不能做 ML-KEM/ML-DSA 运算 |
| Azure Managed HSM | RSA/ECDSA/AES | — | **无** |
| 国密三级机 | RSA/ECC | **SM1/2/3/4** | 厂商称支持 Kyber/Dilithium/FALCON/SPHINCS+ |
| **本项目** | AES-128/256（仅单块） | **SM4、SM3**（无 SM2/ZUC） | **ML-KEM 512/768/1024 + ML-DSA 44/65/87 全参数集，RTL 专用数据通路，逐字节对齐 ACVP、硅上验证** |

**简析**：这是本项目唯一真正拿得出手、且在细分角度领先的维度。差异化不在"有 PQC"（Thales 已认证），
而在**"PQC 跑在专用 RTL 数据通路（NTT/采样/多项式）上、而非通用安全 CPU 的固件里"**。反过来，
传统算法几乎空白（无 RSA/ECC/SM2），做不了绝大多数现网签名/TLS 卸载——产品意义上这是致命的窄。

### 维度 6 — 接口

| | 接口 |
|---|---|
| 商用/国密 | PKCS#11、JCE、OpenSSL engine、KMIP、CNG/KSP；国密另有 **SDF（GM/T 0018）、SKF** |
| **本项目** | **SDF 风格（SDFE_*）+ PKCS#11 v3.2**；无 KMIP、无 SKF |

**简析**：本项目**够得着门槛**——能被标准客户端调用。差在广度与机制集（只 ML-KEM/ML-DSA/AES-GCM）。

### 维度 7 — 性能量级

| | 公开数字 |
|---|---|
| Marvell LS2 | 42,000 RSA-2K/s、**100,000 ECC P-256/s**、1,000,000 GCM/s，35–50 W |
| Thales Luna 7 | >20,000 ECC/s、>10,000 RSA/s |
| YubiHSM 2 | 单次量级：RSA-2048 ~139 ms、ECDSA-P256 ~73 ms（根信任令牌，非批量加速器） |
| **本项目** | **未公开吞吐/时延数字**；`clk_sys` 75 MHz，单板 |

**简析**：**无法对等比较，因为本项目没有发布性能数字**。且上述商用数字几乎都是传统算法，各厂的 PQC
ops/s 很少清晰公开——所以"本项目 PQC 比谁快"没有数据支撑，**不能说**。可说的只是：75 MHz 单板、
面向功能正确性而非吞吐，不属于"高吞吐加速器"这一档；"专用数据通路是否带来更好的性能功耗比"是研究假设，
**需上板实测才能兑现或证伪**。

### 维度 8 — 形态

商用为 1U 机架 / PCIe 卡 / USB nano / 云服务；**本项目是 ALINX AXU3EGB 开发板原型**，非产品形态。
离"能上架"还差一个产品化周期（外壳、供电、防篡改结构、批量固件、认证）。

---

## 4. 量产路径：从原型到真信任根，具体要做什么

写这一节是为了同时挡住两种误读：既不甩锅给硬件（"这颗硅不行"），也不虚报容易度（"量产烧一下就有"）。

### 4.1 硬件与烧写清单（(a) 类，硅已具备）

| 项 | 作用 | 备注 |
|---|---|---|
| `PPK0_HASH` + `RSA_EN` | **启动镜像防替换的认证根**（最小必烧集合） | RSA-4096 + SHA3-384；`PPK1` 留作**唯一一次**吊销备用（eFUSE 只有两组摘要寄存器，细粒度吊销靠 `SPK_ID` 与 `USER` eFUSE） |
| `FUSE_AES` + `ENC_ONLY`，**或** PUF 注册 + 黑钥 | **秘密派生根**（KDR/HUK 要的是这个，与上一行是两件事） | `ENC_ONLY` 只认 eFUSE 里那把 device key，不接受 BBRAM 红钥，故烧它前必须先烧 `FUSE_AES` |
| `USER_0…7` | 单调计数器（可做防回滚锚） | 比"eFUSE 计数器被永久排除"的旧说法乐观：这是量产项，不神秘 |
| `BBRAM_DISABLE` | 永久关掉 BBRAM 这条攻击面 | 官方支持的量产姿态，印证"量产不必依赖 BBRAM" |
| 慎烧：`ERR_DISABLE` | 关掉 JTAG 状态寄存器里的错误信息 | **对本项目代价特别大**：当前排障正是靠读 `CSU_BR_ERROR`（`0xFFD80528`），烧了等于自断调试通路 |
| 易误读：`SEC_LOCK` | 只关"安全锁定时重启进 JTAG"这一条路径 | **不是**"锁死所有安全设置"的总闸；`JTAG_DISABLE` 与 `DFT_DISABLE` 是另外两个独立位 |

PUF 两种模式的产线含义不同：**boot header 模式**零 eFUSE 可跑，但 helper data 与黑钥严格 per-die，
等于"每台机器一份唯一 BOOT.BIN"；**eFUSE 模式**一份镜像通用全 fleet，代价是逐片烧写。
PUF 侧还有一组同样不可逆的位（`SYN_INVALID`/`SYN_WRLK`/`REGISTER_DISABLE`），其中 `REGISTER_DISABLE`
烧下去这颗芯片**再也不能重新注册 PUF**。

### 4.2 OP-TEE 侧：一道会静默降级的闸门（最容易被忽略）

**这是"原型看着跑通、其实没有信任根"的那个陷阱，必须点名。**

上游 `core/drivers/zynqmp_huk.c` 会读 CSU STATUS 的 AUTH 位；**没置起来就静默退化成 development HUK
= 纯 `SHA-256(DNA)`，只 `IMSG` 一行日志**。也就是说：即使烧了 PUF/eFUSE AES，只要认证启动没同时成立，
HUK 依然不是秘密——**而失败形态是"看起来跑通了"**。

好消息是这部分工作量小：`plat-zynqmp/conf.mk` 的依赖链是自动的（`CFG_RPMB_FS` → `CFG_ZYNQMP_HUK` →
`CFG_ZYNQMP_CSU_AES` → `CFG_ZYNQMP_CSU_PUF`），`tee_otp_get_hw_unique_key` 上游已有实现，
**不需要自己写平台移植**。供给到位后 HUK = `SHA-256(DNA + 选中的 USER eFUSE)` 再经 CSU AES-GCM 用
device key 加密——是真正的硬件保护。

**由此纠正一条在本仓库传播最广的归因错误**：`kdr_dna.c:16-18` 等三处写着"OP-TEE 在这颗片子上的 HUK
也是 SHA-256(Device DNA)，性质完全一样"——这被写成了**这颗硅的属性**，实际是**当前未供给形态的属性**。

两条上游 reviewer 点名、尚未解决的残留风险也要记：(1) Linux 侧 AES 驱动能经 CSU 够到同一把 PUF KEK；
(2) MPSoC 规格只保证 PL DNA 的稳定性，PS DNA 没有明确保证，而 HUK 恰恰建在 PS DNA 之上。

### 4.3 代码侧：真正没写的部分（这是"下限"所在）

**别把政策选择的可逆性偷换成工程的已完成度。** 仓库现状：

1. `bbram`/`efuse`/`puf` 三个 KDR provider **一行代码都没有**（`src/crypto/` 下只有 `kdr.c` 与 `kdr_dna.c`）；
2. `cli/pqchsmd.c` 从头到尾**不安装任何 provider**，所以 PRODUCTION 形态的 daemon 今天必然启动失败——
   **哪怕 provider 写好了也照样失败**；
3. provider 选择是 `p11_module.c:564-571` 一处写死的 `strcmp("device-dna")`，没有注册表；
4. 换根后既有 keystore 打不开、**症状与被篡改一致**，迁移路径没有实现；
5. PRODUCTION 形态**从未端到端构建过**。

**但软件侧的准备度其实很高，这点也要如实说**：vtable 形状（`derive(label, salt, …)`）天然就是一次到
安全世界的调用、不需要为硬件根改接口；`tools/check_no_readback.py` 是结构性回归；域分隔、按 slot 分盐、
KEK 轮换都做完且与"根从哪来"正交；PRODUCTION 门是纯标志位判定，测试里的假 provider 今天就在证明
"填 1 即放行"。走 OP-TEE 的话 provider 几乎是纯转接层，量级几十行。**这一块确实是"只是没做"。**

### 4.4 产线与密钥保管（常被完全遗漏的一块）

- **PSK（PPK 对应私钥）必须由一台真 HSM 保管**——Bootgen 有官方 **HSM 模式**，只拿分区哈希去换签名块，
  私钥不落构建机。仓库 `pki/` 下现在那几个明文私钥文件是原型产物，**不能带进量产**。
- 量产 eFUSE/PUF 供给的官方工具 **XLWPT** 需向 AMD 支持代表**按需索取**，不在公开发行包里——这是排产
  计划里的一条外部依赖。
- **良率/返修**：几乎每一位都带 `permanent`，烧错 = 整片报废，且没有"降级回开发态"的通路。产线必须把
  烧写放在**功能测试之后**，并保留一批未烧的返修/调试片。叠加本项目已知的"安全态由 POR 后第一个镜像
  锁定、热重启改不了"，返修可操作空间被进一步压缩。

### 4.5 顺带纠正：防回滚没那么绝望

旧文写成"需要 eFUSE 计数器（永久排除）"，漏了两条：(a) 硅上有 `USER_0…7` 可做单调计数（量产项）；
(b) **今天**就有一条不需要任何烧写、因而在红线之内的路——**eMMC RPMB 的写计数器**，密钥已找回并确证，
`rbanchor` 的 provider 抽象也已经在。不免费（RPMB 读会隔次返回陈帧，要防误判），但这是"没做"，不是"不能做"。

---

## 5. 诚实定位结论

**达到工程原型水准的部分（真东西，站得住）：**
- ML-KEM（512/768/1024）与 ML-DSA（44/65/87）**全参数集在 PL 里逐字节对齐 ACVP、真硅验证**，
  含运行时参数集切换、片内金库。这是扎实的密码核工程。
- 一道**能证明拒绝普通世界读**的 AXI 防火墙边界，加 EL3 SiP + XMPU 毒化的纵深。
- SP 800-90B 风格的 TRNG（8 环振 + RCT/APT + Keccak 调理，阈值由实测熵定）。
- 软件侧的密钥派生骨架（域分隔、分盐、KEK 轮换、no-readback 回归）——与"根从哪来"正交，接真根时不用改接口。

**停在演示/研究级的部分：**
- 管理面软件栈的已登记洞：审计日志正式路径没接、CODE-1 种子过普通世界栈、默认防回滚是文件锚、备份无 epoch。
- 无性能表征，无产品形态，PRODUCTION 形态从未端到端构建。

**产品化必须补的三项，按成因与剩余工作量排序（差一个数量级）：**

| 项 | 类别 | 上限 | 下限（剩余工作量） |
|---|---|---|---|
| **信任根** | **(a)** 硅具备、本项目选择不启用 | 最靠近"只是选择不做" | **最大**：两套独立烧写项 + OP-TEE 供给 + 三个不存在的 provider + 三处没接的装配线 + keystore 迁移 + 产线密钥保管 + 不可逆风险 |
| **防篡改** | **(c)** 需新增产品硬件 | 硅和板都没有 | **未知数最少**：RTL 通路已在，缺 BOM + 机械 + 一次台架验证；采购与工程，不是研究 |
| **资质** | 流程/时间/第三方 | 下游于前两项 | 排序问题而非否定；素材侧已有实体（policy 草稿、ACVP 证据、缺口表） |

**它真正的差异化价值值多少：**

"把 ML-KEM 和 ML-DSA 完整跑进一块廉价 MPSoC 的 PL、专用 RTL 数据通路、逐字节对齐标准向量、且封在一道
会拒绝普通世界的边界里"——这是一个**可信的 PQC 硬件核工程贡献**，在"原生 PQC 于专用硬件通路"这个细分
角度上确实比多数商用 HSM 的"固件加 PQC"更靠前一步。

但要清醒：这是**密码加速核**层面的成就，不是 **HSM 产品**层面的。它为将来在**同一颗 ZU3EG 的烧录形态**
（不必换芯片）或商用安全芯片上做 PQC 硬件加速铺了路。

一句不吹不贬的话：**在"PQC 在硬件里跑通"这一点上，它做到了很多商用产品还没做或刚做的事；在"是不是一台
HSM"这一点上，它还差三个台阶——一个是本项目在现行红线下主动不迈（信任根，硅上都有），一个要新增产品硬件
（防篡改，未知数最少），一个要走认证流程（资质，下游于前两者）。三个台阶都能迈，但没有一个是免费的。**

---

## 6. 待办：文档与代码的自相矛盾（本次未改，需单独处理）

### ✅ 已处理

1. **归因口径已同步到代码。** 四处旧口径（含用户开机可见的运行时字符串）已按本文的四类归因改写：
   `src/crypto/profile.c`（PRODUCTION 拒绝启动消息，现写"本形态未启用秘密硬件根：这颗 ZU3EG 具备
   eFUSE/PUF，但本项目红线不做不可逆烧写；这块板的 BBRAM 另无电池"）、`include/pqchsm/profile.h`、
   `src/crypto/kdr_dna.c`（HUK 那句改成"形态属性非硅属性"，并写明上游 `zynqmp_huk.c` 看 CSU AUTH 位）、
   `boot/atf/patch_atf_secmmio.py`（同源断言，同步改以免口径分叉）。
   已验证：`gcc -fsyntax-only` 在 DEV 与 PRODUCTION 两种 profile 下均通过，
   `check_no_readback` / `check_zeroize` 通过；全仓无测试断言这些文字。
2. **`include/pqchsm/kdr.h` 的 provider 列表已重写**：明确分成"已实现（stub / device-dna）"与
   "规划中，尚无实现（bbram / efuse / puf）"两组，并写明后三者要落地不止是烧片。
3. **RSA 认证启动的证据等级已降级并存档**：新增
   [`boot/persist/RSA-AUTH-BOOT-NOTE.zh-CN.md`](../../boot/persist/RSA-AUTH-BOOT-NOTE.zh-CN.md)。
   **注意结论有变**：原计划"补一份 bif 入库"，考据后**放弃**——不是懒，是重建物会成为伪证。
   决定性理由：`RSA_EN` 未烧时 BootROM 是否认证只取决于启动头 `bh_auth` 位，而 bootgen 对拼错属性
   静默忽略（`build_bbram_boot.sh:131` 自己写着这条）；于是 **G 记录下的每一个可观测量
   （`MULTI_BOOT`=6 / 网络通 / `DAEMON=nosock`），在"认证通过"与"认证被静默跳过"两种世界里逐字相同**。
   要确立这条结论需要的是两个从未做过的**阴性对照**（差分编译验启动头 `0x44` 的 `bh_rsa[15:14]`；
   篡改一字节必须启动失败），而不是一份 bif。存档文档写清了证据在哪（主源是已删除文档
   `git show a516364^:docs/STATUS-2026-08-17.zh-CN.md`）、哪些有据可查、哪些是重建推定。

### 待办（仍未处理）

4. `SECURITY.md:330` 的 "permanently excluded / out of reach for good" 与 L35-36 的"默认拒绝、可经板主同意"
   自相矛盾；建议统一到后者（本次未动 `docs/SECURITY.md`，它是主文档，改动面较大）。
5. PUF 一节的 "settled / never worked" 应改为"成因未定位"，并把 §3 维度 1 注里那个从未做过的组合实验写进去。
6. 六处散文（`README.md:193-194`、`README.zh-CN.md:180-181`、`docs/SECURITY.md:402-403`、
   `docs/SECURITY.zh-CN.md:347` 等）仍把 RSA 认证启动写成既成事实，建议逐处加一句指向上面第 3 条的存档文档。

---

## 来源（2026-08 核实）

**业界产品**
- Thales TCT Luna T-Series FIPS 140-3 L3 + PQC：[thalestct.com](https://www.thalestct.com/hsm-fips140-3-validation/) · [Luna HSM PQC blog](https://cpl.thalesgroup.com/blog/encryption/luna-hsm-pqc-quantum-safe-encryption) · [Luna 8 Network HSM](https://cpl.thalesgroup.com/encryption/hardware-security-modules/network-hsms)
- Thales Luna 7 性能与 tamper：[产品简介 PDF](https://cpl.thalesgroup.com/sites/default/files/content/product_briefs/field_document/2020-04/thales-luna-network-7-hsm-pb-a.pdf)
- YubiHSM 2 FIPS 证书 #5302：[Yubico 新闻稿](https://www.yubico.com/press-releases/yubico-achieves-fips-140-3-validation-for-yubihsm-2-fips-strengthening-hardware-root-of-trust-for-critical-infrastructure/) · [技术数据表 PDF](https://docs.yubico.com/hardware/yubihsm-2/datasheet/_static/YubiHSM_2_Technical_Data_Sheet.pdf)
- Marvell LiquidSecurity 2：[产品页](https://www.marvell.com/products/security-solutions/liquidsecurity2.html) · [Tom's Hardware](https://www.tomshardware.com/news/marvell-unveils-liquidsecurity-2-hsm-up-to-1000000-aes-opss)
- AWS / Azure PQC 现状：[AWS PQC](https://aws.amazon.com/security/post-quantum-cryptography/) · [CloudHSM FIPS 合规](https://docs.aws.amazon.com/cloudhsm/latest/userguide/fips-validation.html) · [云 KMS PQC 就绪度](https://quantumsecuritydefence.com/insights/cloud-kms-pqc-readiness-aws-azure-gcp/)
- 国密密码机：[三未信安服务器密码机](https://www.sansec.com.cn/product/57.html) · [三未信安三级密码卡](https://www.sansec.com.cn/news/31.html) · [数盾（安全三级）](https://www.shudun.com/Product_detials/5.html) · [高性能高安全密码机研究](https://www.secrss.com/articles/15299)

**ZynqMP 量产安全启动（§4 的依据）**
- eFUSE 逐位语义：[XilSKey `xilskey_efuseps_zynqmp_input.h`](https://raw.githubusercontent.com/Xilinx/embeddedsw/master/lib/sw_services/xilskey/examples/xilskey_efuseps_zynqmp_input.h)
- PUF 注册参数：[XilSKey `xilskey_puf_registration.h`](https://raw.githubusercontent.com/Xilinx/embeddedsw/master/lib/sw_services/xilskey/examples/xilskey_puf_registration.h)
- PUF 两种模式与 per-die 含义：[Xilinx Wiki — ZynqMP Security Features](https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/18841708/Zynq+Ultrascale+MPSoC+Security+Features) · [AMD Embedded Design Tutorial — Secure Boot](https://xilinx.github.io/Embedded-Design-Tutorials/docs/2023.1/build/html/docs/Introduction/ZynqMPSoC-EDT/9-secure-boot.html)
- 黑钥/辅助数据须先过 RSA-4096 认证：XAPP1333
- PPK 与 RSA_EN 是 PUF 生效前提：[Foundries.io — Secure Boot on Zynq](https://docs.foundries.io/92/reference-manual/security/secure-boot-zynq.html)
- BBRAM 需 VCC_PSBATT 电池：[DS925](https://docs.amd.com/r/en-US/ds925-zynq-ultrascale-plus) · UG1085
- Bootgen HSM 模式：[UG1283 — Using HSM Mode](https://docs.amd.com/r/2025.1-English/ug1283-bootgen-user-guide/Using-HSM-Mode)
- OP-TEE HUK 桩与 ZynqMP 实现：[`otp_stubs.c`](https://raw.githubusercontent.com/OP-TEE/optee_os/master/core/kernel/otp_stubs.c) · [`zynqmp_huk.c`](https://raw.githubusercontent.com/OP-TEE/optee_os/master/core/drivers/zynqmp_huk.c) · [`plat-zynqmp/conf.mk`](https://raw.githubusercontent.com/OP-TEE/optee_os/master/core/arch/arm/plat-zynqmp/conf.mk) · [PR #4874](https://github.com/OP-TEE/optee_os/pull/4874)

*业界数字为各厂商标称值，可能随型号/固件版本变化，引用日期 2026-08。本项目事实来源：仓库 RTL/代码、
[ARCHITECTURE-TARGET.md](ARCHITECTURE-TARGET.md)、[../SECURITY.md](../SECURITY.md)、[security-policy.md](security-policy.md)。*
