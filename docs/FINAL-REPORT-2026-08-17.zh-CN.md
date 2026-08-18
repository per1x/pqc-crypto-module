[English README](../README.md) · [中文 README](../README.zh-CN.md)

# AXU3EGB 后量子密码机原型 —— 最终状态报告

**日期**：2026-08-17　**器件**：`xazu3eg-sfvc784-1-i`　**分支**：`zu3eg-fpga-crypto`

一句话现状：**密码功能全部在真硅上跑通并对上标准向量；加密启动做不成，
原因已定位到确定的一环并有权威引用；OP-TEE 内存隔离已做成开机自动生效。**

> **2026-08-17 收尾会话补了两件，都做完了**：
> ① XMPU 毒化从"手动 JTAG 步骤"变成**开机自动生效**，现在在默认演示形态里；
> ② 查清了非 golden 槽"时灵时不灵"的根因 —— FSBL 的 **100 秒启动看门狗**
>    加上 WDT 复位后 `multiboot++`。这一条同时把 ① 里那个
>    「`POISON` 回读恒 0」的旧观测证伪了（那份镜像根本没跑起来）。
> 两件的明细在第四节。

口径原则：**每一条"做到了"后面都跟着它没有做到什么。** 没有验证过的一律标成
未验证，不写成"应该可以"。

> 本文是**交付用的结论文档**。逐次实验的原始过程、走过的弯路、被推翻的中间结论，
> 在 [STATUS-2026-08-17.zh-CN.md](STATUS-2026-08-17.zh-CN.md)；那份是日志，这份是结论。

---

## 〇、必须有人动手的事

这三件我做不了，且互相独立。**没有一件会影响当前演示。**

| # | 事项 | 为什么必须是人 | 不做的后果 |
|---|---|---|---|
| 1 | **（又）拔插一次串口线** | `/dev/cu.usbserial-110` 再次回到那个已知的坏状态（`stty` 与 pyserial 都是 `tcsetattr: Invalid argument`，`cu.` / `tty.` 两个节点都是）。拔插那根 Mini-USB（**不是** JTAG 那根）即恢复 | **不紧急**。本轮收尾**全程没有用串口**也把两件事都做完了 —— 诊断改走"写进 XMPU 的草稿寄存器、从普通世界读"，比串口更可靠（见第四节）。JTAG 也一直可用 |
| 2 | **`VCC_BATT` 接一颗纽扣电池** | 板级硬件改动 | BBRAM 加密启动永远不成立（见第四节）。⚠️ **但即使装了也不建议启用**，理由见下 |
| 3 | **决定要不要烧 eFUSE** | 不可逆，且只有一块板；这是产品决策不是技术决策 | 不烧 = **永远拿不到防替换的信任根**。本项目当前的红线是「永不烧」 |

> ⚠️ 关于第 2 条要说清楚：**就算装了电池，也不建议把 BBRAM 加密启动放进交付形态。**
> BBRAM 靠电池维持，电池失效 = 板子起不来；而铁律要求「上电即就绪」。
> 一个"断一次电就变砖"的启动镜像比没有加密启动更糟。
> **PUF 没有这个毛病**（不存钥匙、每次上电现场再生），所以 PUF 本来就是更对的方向 ——
> 只是它卡在别处，见第四节。

---

## 一、最终能力清单

**密码边界是 FPGA fabric。** ML-KEM 核、ML-DSA 核、对称核、环振熵源、片内密钥金库
与访问门控全部在 PL 里。主机侧的 PKCS#11 / SDF 库、keystore、槽位元数据在它**之外**。

| 类别 | 能力 | 证据 |
|---|---|---|
| KEM | ML-KEM-512/768/1024 KeyGen / Encaps / Decaps | 真硅逐字节对 NIST ACVP（20/20） |
| 签名 | ML-DSA-44/65/87 KeyGen / Sign / Verify | 真硅逐字节对 ACVP（32/0，见下方 ⚠️） |
| 对称 | AES-128/256、SM4；ECB/CBC/CTR/CFB/OFB | 对 SP 800-38A / GB/T 32907 |
| 杂凑 | SM3 | 对 GB/T 32905 A.1 |
| 熵源 | PL 环振 + SP 800-90B 健康检测 | 调理前 H = 0.871 bit/sample；PL 重配重启矩阵 H = 0.507 ≥ 0.436 |
| 私钥保护 | 片内金库：`dk`/`sk` 不出总线，按槽号使用 | 读游标 seek 到 sk 段读回全 0；按槽签出的 σ 与自送 sk 逐字节相同 |
| 接口 | GM/T 0018 风格 SDF、PKCS#11 v3.2 | `sdf_demo` 九节全绿 |
| 设备绑定 | 派生根可绑 Device DNA（**默认关**） | 板上直读 + 远端主机经 `OP_DEVICE_DNA` 两条路都跑通 |
| 认证启动 | RSA（`bh_auth_enable`），**零 eFUSE** | 变体 G 上板正常启动、网络通 |

⚠️ **ML-DSA 的 32/0 是在 SK_LOCK 闩上之前取的。** 闩上之后 KeyGen 只吐 `pk`，
ACVP 的 KeyGen 向量比对不了私钥 —— 那 6 条**必然**失败，**那是闩锁在正常工作**。
要重取 KeyGen 证据：先重配 PL 清掉闩锁，且别让 daemon 先起来。

### 明确**没有**的东西

* **没有加密启动**（第四节，结论是确定性的）
* **没有防替换的信任根** —— 需要烧 eFUSE，永久排除
* **root 仍然能"用"密钥**（读不到私钥，但能命令硬件做运算 —— SiP 不校验调用来源）
* ~~OP-TEE 安全内存的隔离只在运行时~~ —— **本轮已解决**，现在开机自动生效（第四节）。
  但它挡住的**只是"普通世界读 OP-TEE 的 core 段"这一条**，上面那条"root 仍然能用密钥"不变
* **没有 SM2**
* **没有算法证书**（ACVP 向量是本地跑的，那是正确性证据，不是证书）
* **没有功耗/电磁侧信道对策**（刻意排除，理由见 [SECURITY](SECURITY.zh-CN.md)）

---

## 二、演示形态 vs 送检形态

| | 演示形态（**默认**） | 送检形态 |
|---|---|---|
| 位流 | `zu3eg_hsm_dev.bit`，`SECURE_ONLY=0` | `zu3eg_hsm.bit`，`SECURE_ONLY=1` |
| 装载时机 | 开机后由 `hsm-boot.sh` 运行时装 | 同左 |
| 普通世界能否直接读密码核 | 能（`/dev/mem`） | **不能**（防火墙 RAZ/WI），只能经 `/dev/secmmio` → EL3 白名单 |
| 网络 | eth0 随 PL 消失；**eth1（PS GEM）不受影响** | 同左 |
| 上电到就绪 | 约 35 秒，全自动 | 同左 |
| XMPU 毒化 | **默认开，开机自动生效**（BL31 在 EL3 里写，无手工步骤） | 同左 |

**默认就是演示形态**：上电 35 秒后 daemon 起好、`fpga_manager` 报 `operating`，
不需要任何人工操作。

⚠️ `hsm-boot.sh` 的顺序是死的：**先配好 eth1 并确认有载波，再碰 PL**。
没载波就跳过位流装载 —— 换位流会把 eth0 一起弄没，两条网都断就失联了。

⚠️ **白名单少一槽 = 那个核在送检形态下等于不存在。** 加从机要同时改
`boot/atf/patch_atf_secmmio.py` 里的 `pl_rd_ok[]`（当前 7 槽：`{1,1,1,1,1,0,1}`）。

---

## 三、资源与时序

`xazu3eg-sfvc784-1-i` 完整布局布线：

| | |
|---|---|
| CLB LUT | 35,659 / 70,560（**50.54 %**） |
| 寄存器 | 25,977（18.41 %） |
| Block RAM | 15.5 / 216（7.18 %） |
| DSP | 140 / 360（38.89 %） |
| WNS @ 75 MHz | **+3.325 ns** |
| 保持余量 | **+0.110 ns** |

⚠️ 保持余量 0.110 ns 很薄，**RTL 一改就要重新核对**；实现流程在低于 0.050 ns 时中止。
保持违例不能靠放慢时钟解决。

---

## 四、done / blocked 明细

### ✅ done

| 项 | 证据 |
|---|---|
| ML-KEM / ML-DSA / AES / SM4 / SM3 / TRNG / KEM-DEM 全部硬件跑通 | 真硅对标准向量，日志在 `board/logs/` |
| 片内密钥金库 + SK_LOCK 闩锁 | 读游标反证 + 按槽签名一致性 |
| 设备绑定（Device DNA，默认关） | `board/logs/RESULT_dna_bind.txt`；写会被 EL3 拒也是正面验过的 |
| rtl_sim 回归 | 主机 47/47；rtl_sim 全量 251 用例全过 |
| golden 完整性 | 四份镜像与板外副本逐字节一致，`board/logs/BOOT_MANIFEST.txt` |
| **RSA 认证启动（零 eFUSE）** | 变体 G 从槽 6 正常启动、网络通 |
| **XMPU 毒化：开机自动生效** | `board/logs/RESULT_xmpu_persist.txt`：上电即 Bus error；关掉后重启它自己回来；十二个采样点全 `0x00100000` |
| **槽启动"时灵时不灵"的根因** | `board/logs/RESULT_slot_boot_wdt.txt`：FSBL 100 秒看门狗 + WDT 复位 `multiboot++`；`LOG_LEVEL=20` 后三份镜像零失败 |
| keystore 回滚防护、tamper 网表断言、全 cipher 模式、SDF/PKCS#11 覆盖表 | 见 STATUS 第三节 ⑤ |

### ⛔ blocked：加密启动 —— **根因已定位到具体一环**

**先把天花板写死**：不烧 eFUSE 就没有 `ENC_ONLY` 和 `RSA_EN`，**BootROM 不强制加密、
也不强制认证**。攻击者换一份"声明为不加密"的镜像照启不误。
**所以本项目在任何配置下都拿不到防替换的信任根 —— 这不是失败，是芯片的规则。**

#### BBRAM：受阻于板级硬件

两个必要条件互斥：烧钥匙会把 `BBRAM_STS.PGM_MODE` 锁成 1（CSU 就不把钥匙交给
BootROM），而清这个锁存要 POR，POR 又会把钥匙一起清掉（`VCC_BATT` 没电池）。

⚠️ **本轮更正了这条的推理**：原来写的是"只有物理断电能清 `PGM_MODE`，而软件没有
POR 通路"—— **后半句是错的**（第五节）。真正的死因是**钥匙撑不过真实断电，
与"上电即就绪"不相容**。结论不变，理由变了。

**代价**：要人给 `VCC_BATT` 接电池；且即便接了也不建议启用（见第〇节）。

#### PUF 黑钥：BootROM **在碰 PUF 之前**就拒收

这条查得最深，结论是**确定性的**：

| 环节 | 状态 | 依据 |
|---|---|---|
| 启动头各字段（密钥来源 `0xA35C7C53`、PUF_HD、shutter、黑钥、黑钥 IV） | ✅ 全对 | 逐字对过 UG1085 表 11-4 / 11-5 |
| 辅助数据 386 字 | ✅ 与 XilSKey `SyndromeData[]` 逐字一致 | 含修正末字为 `AUX<<4` = `0x864FE200` |
| 镜像的 AES-GCM 加密 | ✅ **离线解密 + tag 校验通过** | 用红钥 + 启动头 IV 解 FSBL 段，明文正是 ZynqMP 安全头 |
| RSA 认证通路 | ✅ **上板验过是通的** | 变体 G |
| 安全态规则（错误码 `0x53`） | ✅ **已用 POR 冷启动绕开** | 见第五节 |
| 镜像属性保留位（错误码 `0x33`） | ✅ **已用变体 H 排除** | 去掉 `puf4kmode` 后行为不变 |
| **BootROM 接受 `bh_blk_key` 这个密钥来源** | ❌ **就卡这里** | 见下 |

**决定性证据（JTAG 读 CSU 内部状态，APU 停着也能读）**：

```
CSU_PUF_CMD    (0xFFCA4000) = 0x00000000   BootROM 从没发过 PUF 命令
CSU_PUF_CFG1   (0xFFCA4008) = 0x00080080   复位默认（4K 应为 0x0c230090）
CSU_PUF_SHUT   (0xFFCA400C) = 0x01000020   复位默认（启动头里写的是 0x0100005E）
CSU_PUF_STATUS (0xFFCA4010) = 0x00000002   KEY_RDY = 0，从没再生出密钥
CSU_ISR PUF_ACC_ERROR       = 0            连一次 PUF 访问都没发生
OCM 0xFFFC0000              = DEADBEEF…    OCM 初始化填充，FSBL 压根没装载
```

> **PUF 再生、黑钥解密、辅助数据、shutter —— 这些从头到尾就没被用到过。**
> 卡点在更早的启动头校验。

**剩下两种解释，都不是我们这边能动的，且都指向 eFUSE**：
① 这颗硅/这版 BootROM 不接受启动头黑钥；
② 存在未公开的 eFUSE PUF 前置条件 —— 注意 **Xilinx 自己的
`XilSKey_Puf_Regeneration()` 开头就拒绝 eFUSE CHASH 为 0**，理由写着
*"PUF regeneration is not allowed, as PUF data is not stored in eFuse"*，
**这与 UG1085 表 12-6「CHASH 未编程时可用启动头黑钥」自相矛盾**（Xilinx 的文档
和自己的库对不上）。

**代价**：要突破只有两条路 —— 烧 eFUSE（永久红线），或找 Xilinx 支持确认这颗硅
是否真的支持启动头黑钥。**按项目纪律「没有依据就不再猜着上板」，到此为止。**

**留下的可复用资产**：`boot/persist/build_puf_boot.sh`（生成并自检加密镜像）、
`/home/build/pufboot/`（各变体镜像、bif、psk/ssk、辅助数据）。

### ✅ done（本轮补齐）：XMPU 隔离 —— **开机自动生效，不再需要任何手工步骤**

> **本轮改动**：上一版这里写的是"能做到，但只在运行时"。**那条已经不成立了。**
> 现在 BL31 在 EL3 里开机就把六个 XMPU_DDR 的 POISON 写好，
> `devmem 0x60000000` 从上电起就是 Bus error，**没有 JTAG 步骤、没有手工动作**。
> 证据：[`board/logs/RESULT_xmpu_persist.txt`](../board/logs/RESULT_xmpu_persist.txt)。

**先更正两条旧结论**（旧文档里都是错的）：

1. ~~"挡住了非安全 DMA 主控 `0xa0`/`0xac`"~~ —— **那两个主控号就是 APU**
   （PMUFW `xpfw_xpu.c:71`：`{0x80, 0xBF, "APU"}`）。**XMPU 此前一个访问都没挡住。**
2. `protunit_probe` 把 `+0x10` 当 LOCK 打印，**其实那是 ISR**（LOCK 在 `+0x20`）。
   历史日志里的 `LOCK=0x8` 是 `ISR.SECURTYVIO`。工具已修正。

**根因**（XAPP1320 v3.0 第 10 页）：

> "the XMPU asserts AxUser[10] but the transaction **is passed to the memory
> controller** … **The transaction is gated by the end point, not the XMPU itself.**"

**XMPU 只检测、只打毒标记，掐掉动作由终点做。**
本项目原先 `POISON = 0`，所以那时**只有检测**——这正是"配了 XMPU 却一个访问都没挡住"的原因。
（现在 `POISON.ATTRIB = 1`，见下。）

**开了毒化之后，板上实测有效**：

```
毒化关   devmem 0x60000000 → 0xAA0003F3         OP-TEE 的一条 AArch64 指令
毒化开   devmem 0x60000000 → Bus error（SIGBUS）
对照     devmem 0x10000000 → 0xEDFE0DD0         区域外，照常
对照     devmem 0x70000000 → 照常                共享内存，故意不圈
功能     sdf_demo 九节全绿；网络 / PL / daemon 均不受影响
```

#### 「开机自动生效」——本轮做成了，而且根因和之前想的完全不是一回事

上一版这里列了三条写入路"全不通"，其中第一条是：

> ~~BL31 在启动时写 → 区域寄存器写得进去，但同一循环里的 `POISON` 写不生效，回读恒 0~~

**这条观测本身是假的。那份带 `POISON` 写的镜像根本没跑起来。**
它被 FSBL 的 100 秒启动看门狗打掉、`multiboot` 自增到 7，
实际在跑的是槽 7 里另一份**不含这段代码**的 BL31 —— 读到 0 理所当然。
详见下面「查清槽启动不稳定性」一节。

把 `LOG_LEVEL` 从 50 降到 20 之后镜像真的跑起来了，于是用**三个采样点**
（写进被禁用区域 R14 的三个寄存器当草稿纸，经 SiP 只读窗口从普通世界读，
**不用串口也不用 JTAG**）一次夹死：

| 采样点 | 位置 | 读到 |
|---|---|---|
| ① `R14_START` | 早期那次写完，紧接着回读 | `0x00100000` |
| ② `R14_MASTER` | BL31 交接前（早期那次还在不在） | `0x00100000` |
| ③ `R14_END` | 交接前再读一次 | `0x00100000` |
| ④ POISON 现值 | Linux 起来之后 | `0x00100000` |

两个诊断版镜像 × 六个实例 × 三个采样点，**十二个点全是 `0x00100000`**。

> **所以：EL3 那一笔写从来就是成功的，也从来没有被谁清掉。**
> 「EL3 写没进去」和「写进去又被清掉」**两个都不是** —— 是那份镜像压根没跑。

顺带证明了"交接前再写一次"是多余的：去掉它的对照版（`8a290f42…`）
四个采样点一模一样。所以生产版**不带**任何重写、也不带任何诊断写
（`patch_atf_secmmio.py` 里 `PQCHSM_XMPU_LATE` 默认 0）。

**现在的状态 —— 开机即生效，零手工步骤**：

```
上电 → 等 35 秒 → devmem 0x60000000 → Bus error      ← 什么都不用做
                   devmem 0x10000000 → 0xEDFE0DD0     ← 区域外，照常
                   devmem 0x70000000 → 照常           ← 共享内存，故意不圈
```

同一次启动内的关/开对照（判据只认同址前后对照）：

```
JTAG 把 POISON 清零   devmem 0x60000000 → 0xAA0003F3   ← OP-TEE 的一条 AArch64 指令
直接重启，什么都不做   devmem 0x60000000 → Bus error    ← BL31 自己又写回去了
```

`protunit_probe` 同时显示 DDR1/DDR2（APU 到 DDR 的两个 CCI 端口）
`ERR_MASTER=0xa0(APU)`、`ISR=0x8[SECURTYVIO]` —— **检测和拦截现在都成立**。

**功能影响：零。** `sdf_demo` 九节全绿、`dna_probe -w` 正常、eth1 通、PL `operating`、
上电到就绪仍是约 35 秒。

其余两条路的结论不变，仍然记在这里：

| 路径 | 结果 |
|---|---|
| PMU 的 `PM_MMIO_WRITE`（EEMI #19，与 `pmsec.ko` 写 multiboot 同一条路） | **PMU 拒绝，返回 `2002 = XST_PM_NO_ACCESS`**（六个实例全拒） |
| EL3 SiP 写口 | 白名单只放行 PL 段；放开它等于让普通世界能改内存保护配置，**不做** |

⚠️ **仍然要写清的边界**：
* 挡住的只是**普通世界读 OP-TEE 的 core 段**这一条。**root 仍然能"用"密钥**
  （SiP 不校验调用来源），共享内存段本来就该双向可读，也没有防替换的信任根。
* **没做真 POR 复验**：`jtag_por.tcl` 要求 PMU 的 CSU_ROM 错误位置着才能触发，
  而本轮 `ERROR_STATUS_1/2` 都是 0。为凑触发源去故意搞坏一次启动，风险大于收益。
  已验证的是**等价冷路径**（`MULTI_BOOT=0`、从 `BOOT.BIN` 起，整条链与上电相同）；
  XMPU 配置由 BL31 每次启动重写，不依赖任何 POR-only 状态。
* 万一新 `BOOT.BIN` 某次上电起不来：FSBL 会 `multiboot++` 落到 `BOOT0001.BIN`
  （`bb3402ea`，已知 10/10 能启动）—— 板子仍起得来，但**那一份不带开机毒化**。

证据：[`RESULT_xmpu_persist.txt`](../board/logs/RESULT_xmpu_persist.txt)（本轮，以这份为准）、
`RESULT_protunit_v2.txt`（实配）、`RESULT_xmpu_poison.txt`（旧的运行时对照）。

### ✅ done（本轮补齐）：查清槽启动不稳定性 —— **一条 100 秒的死线**

问题原话：「上次重建镜像能从槽 6 启动、这次却落到 `MULTI_BOOT=7`，原因没查清。」

**根因**：本项目自己的 FSBL 补丁（`make_wdt_patch.py`）第 4 行写着
*"fall back (**multiboot++**) on any WDT-induced reset"*。链条是：

1. FSBL 交接给 ATF **之前**武装 PS SWDT0，`XFSBL_WDT_EXPIRE_TIME = **100** 秒`；
2. 而且**故意不在 handoff 时停掉它**（原文：*"the WDT is intentionally NOT stopped
   at handoff … It keeps guarding the ATF/OP-TEE/U-Boot phase"*）；
3. 停狗的动作在 **U-Boot 的 `boot.scr`** 里；
4. 于是「FSBL 交接」到「U-Boot 跑到 `boot.scr`」之间有一条**硬的 100 秒线**；
5. 超了就整机复位，下一次 FSBL 一看是 WDT 引起的复位就 **`multiboot++`**。

而 `LOG_LEVEL=50` 让 BL31 把整张页表（2888 行）打到 115200 的串口，
**把 ATF 那一段推到了 100 秒线附近** —— 赶上了就起来，没赶上就 6→7。
**"时灵时不灵"是在跟一条 100 秒的死线赛跑，不是 BL31 代码的问题。**

板上量到的数：

| 镜像 | 槽 6 | 网络回来 | `CSU_MULTI_BOOT` |
|---|---|---|---|
| `c4c99e2e`（已知能启动，对照） | 成功 | 20 秒 | 6 |
| `12c3fc8e`（`LOG_LEVEL=50`，复现旧失败） | **失败** | **138 秒** | **7** |
| `3771d140` / `8a290f42` / `0ea6e443`（`LOG_LEVEL=20`） | 全部成功 | 23 / 41 / 35 秒 | 全部 6 |

138 ≈ 100（看门狗）+ 38（落到槽 7 的一次正常启动），对得上。

**排除 BootROM 的两条硬证据**：
* `CSU_BR_ERROR = 0x00000000` —— **BootROM 根本没报错**；
* 两份镜像的**启动头 14 个字段、六个分区头逐字节相同**，
  46845 字节差异**全部**落在 BL31 那个分区内部（0x47256c..0x484ca9）。
  而 BootROM 只校验启动头 + 装载 FSBL，这两样两份完全一样。
  分区头里 checksum word offset = 0，**FSBL 也不校验分区内容**。

⚠️ **别再把 `LOG_LEVEL` 调回 50。** 真要看 VERBOSE，得先放宽 FSBL 的 100 秒
或让 U-Boot 更早停狗，否则就是在自找 `multiboot++`。

证据：[`RESULT_slot_boot_wdt.txt`](../board/logs/RESULT_slot_boot_wdt.txt)。

### 其他 blocked / 不做

| 项 | 状态与理由 |
|---|---|
| SM2 | 未做，范围外 |
| 算法证书 | 没有。ACVP 向量本地跑，是正确性证据不是证书 |
| 操作粒度 SiP | **不做**（用户拍板）。EL3 没有异常处理，把硬件时序搬进去 = 一次总线拒绝就得断电 |
| 侧信道对策 | 刻意排除：没有实验台就无法证明掩码有效，交付未验证的掩码比不做更糟 |
| J1（位流固化进 BOOT.BIN） | 能启动但那个形态掉网。唯一消费者是加密启动，而加密启动已 blocked，**不是必需项** |

---

## 五、本轮推翻的旧结论（防止有人拿旧文档）

这几条以前写在文档/记忆里，**现在确认是错的**：

| 旧说法 | 事实 |
|---|---|
| "PUF 黑密钥依赖烧 eFUSE，已永久排除" | **错**。`bh_auth_enable` 的语义就是跳过 eFUSE PPK 校验，这条路不需要烧 eFUSE |
| "这颗芯片没有软件 POR 通路，只能物理断电" | **错**。PMU 的错误处理逻辑可配成产生 POR：`mwr 0xFFD80560 0x04000000`（`ERROR_POR_EN_2` bit26 = CSU_ROM）。用 `PERS_GLOB_GEN_STORAGE7` 验证过是真 POR。脚本 `board/scripts/jtag_por.tcl` |
| "BootROM 认证失败硬停之后 JTAG 救不回来" | **错**。真因是启动模式被切成 JTAG（`BOOT_MODE_USER.USE_ALT`）。救板配方要加一行 `mwr 0xFF5E0200 0x0` |
| "XMPU 挡住了非安全 DMA 主控" | **错**。那些主控号是 APU；XMPU 当时什么都没挡住 |
| "BootROM 那步没有公开资料" | **错**。UG1085 第 11 章有完整错误码表，且错误码可读：`PMU_GLOBAL.CSU_BR_ERROR` = `0xFFD80528` |
| "非 golden 槽 + 热重启"可以测加密启动 | **错**。安全态由 POR 后第一个镜像锁定（错误码 `0x53`），热重启永远测不了加密启动 |
| "BL31 在 EL3 里写 XMPU 的 `POISON` 写不进去，回读恒 0" | **错，而且是假观测**。那份镜像根本没跑起来（被 FSBL 的 100 秒看门狗打掉、`multiboot++` 落到槽 7），读的是另一份 BL31 的 `POISON`。真跑起来之后，十二个采样点全是 `0x00100000` |
| "重建的 BL31 起不来网络" | **错**。它起得来，只是**慢**：`LOG_LEVEL=50` 把启动推过 FSBL 的 100 秒看门狗线。降到 20 之后三份镜像零失败 |
| "槽 6 时灵时不灵，原因不明" | **查清了**。不是玄学，是在跟一条 100 秒的死线赛跑（FSBL 武装 SWDT0、故意不在 handoff 停、U-Boot 才停狗） |
| "XMPU 隔离只能在运行时靠 JTAG 打开" | **错**。已做成开机自动生效，且**不需要**交接前重写、不需要 PMU、不需要放开 SiP 写口 |
| "`/home/build/wdt_patch/images/BOOT_SECMMIO.BIN` 是在跑的 `BOOT.BIN` 的离板副本" | **已失效**。它在上一轮又被重建覆盖了（现为 `12c3fc8e`）。真正的副本是 `/home/build/BOOT_WORKING_REF.BIN`；当前 `BOOT.BIN` 的副本在 `/home/build/keep/` |

---

## 六、怎么演示

**上电，等 35 秒。** 不需要任何操作 —— `fpga_manager` 报 `operating`、daemon 起好、
eth1 在 `192.168.50.175`。

```bash
# 本机（板子上）
/media/sd-mmcblk1p2/hsm/sdf_demo

# ⚠️ 这两行 2026-08-18 起**已经不成立**：远程口换成了 mTLS。见文末附录。
# 远程（另一台机器，先从板子取一次性口令）
TOK=$(ssh root@192.168.50.175 cat /media/sd-mmcblk1p2/hsm/hsm_token)
./sdf_demo 192.168.50.175 "$TOK"
```

九节内容：PING / ML-KEM-768 三步 / SM4 密钥进硬件 / **ML-DSA 三个参数集** /
四种分组模式 / SM3 / 以及一条**反证** —— 断开重连后旧句柄必须失效。

| 单项工具（都在 `/media/sd-mmcblk1p2/hsm/`） | 演示什么 |
|---|---|
| `dna_probe -w` | 设备绑定：读 DNA，并**正面验证写会被 EL3 拒绝** |
| `protunit_probe` | 保护单元实配（含主控 ID 名字与 `ISR[SECURTYVIO]` 解码） |
| `mldsa_hwtest` | ML-DSA 对 ACVP（注意 SK_LOCK，见第一节 ⚠️） |
| `dna-bind-check`（主机侧） | 远端主机把 keystore 绑到这块板 |

**想演示 OP-TEE 内存隔离** —— **不用做任何准备，开机就是生效的**：

```bash
# 板上，root 身份。第一条应当 Bus error，后两条应当照常返回。
devmem 0x60000000 32     # OP-TEE 的安全内存 → Bus error
devmem 0x10000000 32     # 区域外           → 0xEDFE0DD0
devmem 0x70000000 32     # OP-TEE 共享内存   → 照常（故意不圈）
```

演示时值得指出的一句：**这三条是同一个 root 用户跑的**。
读不到 `0x60000000` 不是因为权限不够，是因为 XMPU 把那笔访问毒化了。

想把对照做全（可选）：用 JTAG 把六个实例的 `POISON` 清零，
`devmem 0x60000000` 就会读回 `0xAA0003F3`（OP-TEE 的一条 AArch64 指令）——
**这正是"没有 XMPU 时 root 能读到安全内存"的反证**。
清零之后直接重启就恢复，BL31 会自己写回去。

⚠️ `board/scripts/xmpu_poison_on.tcl` 现在**只是个应急/演示工具**，
不再是必需步骤 —— 默认镜像已经把它做的事在启动时做完了。

### 掉网了怎么办

1. 先用**网络**看状态；串口只做**只读**观察，**任何会写/会刷的命令一律走网络**
   （串口会把发出去的字符弄重复，已有事故记录）。
2. JTAG 救板（**本轮用了十余次，一次电都没断**）：

```tcl
connect -url tcp:127.0.0.1:3121
targets -set -filter {name =~ "PSU"}
mwr -force 0xFF5E0200 0x0      ;# ★ 清 USE_ALT —— 少了这行会误以为救不回来
mwr -force 0xFFCA0010 0        ;# CSU_MULTI_BOOT = 0 → 回 golden
rst -system
after 8000
targets -set -filter {name =~ "Cortex-A53 #0"}
catch {con}                    ;# 会停在 Reset Catch，必须 con
```

3. 真需要 POR 时用 `board/scripts/jtag_por.tcl`（软件触发，不用拔电源）。

---

## 七、板上当前状态

| 文件 | md5 | 说明 |
|---|---|---|
| `p1/BOOT.BIN` | `0ea6e4437698abc7b44b3a235b49ce78` | **当前默认镜像（本轮更新）**。= 旧的那份 + `LOG_LEVEL=20` + XMPU 毒化开机生效 |
| `p1/BOOT_PRE_XMPU.BIN` | `c4c99e2e5516879e11b7c1364b21f2fa` | **换上去之前的 `BOOT.BIN`**，留在卡上当回退；离板副本 `/home/build/BOOT_WORKING_REF.BIN` |
| `p1/BOOT0002.BIN` = `BOOT_GOLDEN.BIN` | `8d42d1a58b62c8ca95501bf486fbb45d` | **兜底 golden，本轮一个字节没动**，与板外副本一致 |
| `p1/BOOT0001.BIN` | `bb3402ea4efae7e1062a65632fe684b4` | 已知 10/10 能启动 —— 也是 `BOOT.BIN` 失败时 FSBL 自增会落到的那一个 |
| `p1/BOOT0007.BIN` | `2d7799a32bbf3a01a6e61664b6241198` | J1 镜像（能起但掉网）。本轮实验期间临时借用过，**已还原** |
| `p1/BOOT0006.BIN` | —— | **已删除，保持删除** |

`CSU_MULTI_BOOT = 0`，PL `operating`，eth1 `192.168.50.175`，`sdf_demo` 九节全绿。

**离板副本**（都在构建机上）：

| 路径 | md5 | 是什么 |
|---|---|---|
| `/home/build/keep/BOOT_SECMMIO_XMPU.BIN` | `0ea6e443…` | 当前 `BOOT.BIN` 的副本，**已置只读** |
| `/home/build/keep/bl31_secmmio_xmpu.elf` | `312a8c09…` | **当前 `BOOT.BIN` 所用的那份 BL31 的 ELF**，已置只读 |
| `/home/build/BOOT_WORKING_REF.BIN` | `c4c99e2e…` | 上一版 `BOOT.BIN` |

> ⚠️ 上一轮丢过一次 `bl31.elf`（就地重建覆盖、没有备份）。本轮的对策写进了
> `build-bl31-secmmio.sh`：**每次换 `BUILD_BASE` 和输出名，不覆盖上一版**，
> 并且成品另存一份到 `/home/build/keep/` 且置只读。
> 「要改 BL31 就得先解决重建镜像与在跑的这份不一致」这个遗留问题**已经消失** ——
> 现在在跑的这份就是从当前 ATF 树重建出来的，脚本、源码、产物三者对得上。

⚠️ 卡上还有一个**与本轮无关的既有状态**，记一笔免得以后当成新问题：
`p1/use_backup` 这个标记文件存在，于是 `boot.scr` 一直走
`image_backup.ub` 而不是 `image.ub`（两者同尺寸同日期）。演示不受影响，
但**要换内核就得同时换 `image_backup.ub`，或者先删掉 `use_backup`**。

---

# 附录（2026-08-18 追加）：这份报告里已经过时的三处

本报告是 **2026-08-17** 的快照，**正文不改**。以下三处在 2026-08-18 变了，
以这里为准（完整口径见 `docs/SECURITY.zh-CN.md` 的 2026-08-18 一节）：

1. **远程口不再是"明文 TCP + 一次性口令"，改成了 mTLS。**
   正文里那两行
   `TOK=$(ssh root@192.168.50.175 cat /media/sd-mmcblk1p2/hsm/hsm_token)` /
   `./sdf_demo <IP> "$TOK"` **已经不成立**。现在是：

   ```bash
   ./tools/demo_remote.sh --provision   # 一次性：生成 PKI、装板子、留凭据、对时
   ./tools/demo_remote.sh --smoke       # 之后纯本地，零 SSH
   ```

   板上 `pki/` 里 `hsm_ca.crt` / `hsm_device.crt` / `hsm_device.key` 缺一就不监听
   TCP（fail-closed）。旧的 `hsm_token` 文件已经从板上删掉，留着它只会误导。

2. **keystore 的防回滚锚点抽象成了 provider**（`pqchsm/rbanchor.h`）。默认仍是
   `<keystore>.epoch` 文件，**仍然不是真正的防回滚**；RPMB 那条实现已经写好，
   但**这块板的 RPMB 认证密钥已被烧掉且密钥丢失**，这条路在本机永久关闭。

3. **多了一个构建形态开关 `PQC_PROFILE`**。默认 `DEV`（允许桩 KDR，启动打告警）；
   `PRODUCTION` 编译期就把桩根密钥去掉，并要求 `hardware_backed` 的 KDR ——
   **这块板做不到，所以 PRODUCTION 构建会如实拒绝启动**。

另有四条 P0/P1 软件缺陷在 2026-08-18 修掉（liboqs 全局随机源并发串扰、
自测闸门可被并发旁路、keystore fail-open + 退出覆盖、密钥注入非事务、
会话 close 的 ABA、安全状态不抗掉电），每一条都带一条**在修复前确实会红**的回归。
