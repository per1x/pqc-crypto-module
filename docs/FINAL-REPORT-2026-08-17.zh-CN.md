[English README](../README.md) · [中文 README](../README.zh-CN.md)

# AXU3EGB 后量子密码机原型 —— 最终状态报告

**日期**：2026-08-17　**器件**：`xazu3eg-sfvc784-1-i`　**分支**：`zu3eg-fpga-crypto`

一句话现状：**密码功能全部在真硅上跑通并对上标准向量；加密启动做不成，
原因已定位到确定的一环并有权威引用；OP-TEE 内存隔离能做到但只在运行时。**

口径原则：**每一条"做到了"后面都跟着它没有做到什么。** 没有验证过的一律标成
未验证，不写成"应该可以"。

> 本文是**交付用的结论文档**。逐次实验的原始过程、走过的弯路、被推翻的中间结论，
> 在 [STATUS-2026-08-17.zh-CN.md](STATUS-2026-08-17.zh-CN.md)；那份是日志，这份是结论。

---

## 〇、必须有人动手的事

这三件我做不了，且互相独立。**没有一件会影响当前演示。**

| # | 事项 | 为什么必须是人 | 不做的后果 |
|---|---|---|---|
| 1 | **拔插一次串口线** | `/dev/cu.usbserial-110` 处在已知的坏状态（`stty` 回 `tcsetattr: Invalid argument`）。拔插那根 Mini-USB（**不是** JTAG 那根）即恢复 | 掉网时少一条只读兜底。本轮全程没用上串口也做完了，所以不紧急 |
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
* **OP-TEE 安全内存的隔离只在运行时**（第四节 XMPU 那条）
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
| XMPU 毒化 | **默认关**（要手动开，见第四节） | 同左 |

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
| **XMPU 毒化（运行时）** | `board/logs/RESULT_xmpu_poison.txt`，关/开同址对照 |
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

### ⛔ blocked：XMPU 隔离 —— **能做到，但只在运行时**

**先更正两条旧结论**（旧文档里都是错的）：

1. ~~"挡住了非安全 DMA 主控 `0xa0`/`0xac`"~~ —— **那两个主控号就是 APU**
   （PMUFW `xpfw_xpu.c:71`：`{0x80, 0xBF, "APU"}`）。**XMPU 此前一个访问都没挡住。**
2. `protunit_probe` 把 `+0x10` 当 LOCK 打印，**其实那是 ISR**（LOCK 在 `+0x20`）。
   历史日志里的 `LOCK=0x8` 是 `ISR.SECURTYVIO`。工具已修正。

**根因**（XAPP1320 v3.0 第 10 页）：

> "the XMPU asserts AxUser[10] but the transaction **is passed to the memory
> controller** … **The transaction is gated by the end point, not the XMPU itself.**"

**XMPU 只检测、只打毒标记，掐掉动作由终点做。** 我们的 `POISON = 0`，所以只有检测。

**开了毒化之后，板上实测有效**：

```
毒化关   devmem 0x60000000 → 0xAA0003F3         OP-TEE 的一条 AArch64 指令
毒化开   devmem 0x60000000 → Bus error（SIGBUS）
对照     devmem 0x10000000 → 0xEDFE0DD0         区域外，照常
对照     devmem 0x70000000 → 照常                共享内存，故意不圈
功能     sdf_demo 九节全绿；网络 / PL / daemon 均不受影响
```

**但"开机自动生效"没做成 —— 三条写入路全试过，全不通**：

| 路径 | 结果 |
|---|---|
| BL31 在启动时写 | 区域寄存器写得进去（`SECURTYVIO` 会锁存），**但同一循环里的 `POISON` 写不生效**，回读恒 0 |
| PMU 的 `PM_MMIO_WRITE`（EEMI #19，与 `pmsec.ko` 写 multiboot 同一条路） | **PMU 拒绝，返回 `2002 = XST_PM_NO_ACCESS`**（六个实例全拒） |
| EL3 SiP 写口 | 白名单只放行 PL 段；放开它等于让普通世界能改内存保护配置，**不做** |

**代价与现状**：目前它是一条**手动 JTAG 步骤**（`board/scripts/xmpu_poison_on.tcl`，
幂等），**不在默认演示形态里**。要变成开机生效，需要能在 EL3 或 PMU 侧写通
POISON —— 那要动启动链，而在唯一一块板上反复动启动链的风险与收益不划算。

⚠️ **口径**：BL31 里那份区域配置**留着**（它是可观测的检测），但文档里必须同时写明
**默认情况下它一个访问都没挡住**，否则就是夸大。
证据：`board/logs/RESULT_protunit_v2.txt`（实配）、`board/logs/RESULT_xmpu_poison.txt`（对照）。

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

---

## 六、怎么演示

**上电，等 35 秒。** 不需要任何操作 —— `fpga_manager` 报 `operating`、daemon 起好、
eth1 在 `192.168.50.175`。

```bash
# 本机（板子上）
/media/sd-mmcblk1p2/hsm/sdf_demo

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

**想演示 OP-TEE 内存隔离**（可选，手动一步）：

```bash
sudo -H -u build bash -lc "source /tools/Xilinx/Vitis/2020.1/settings64.sh; xsct xmpu_poison_on.tcl"
# 然后在板上：devmem 0x60000000 32 → Bus error；devmem 0x10000000 32 → 照常
```

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
| `p1/BOOT.BIN` | `c4c99e2e5516879e11b7c1364b21f2fa` | 当前 golden（含 DNA 只读窗口、保护单元只读窗口、XMPU 区域） |
| `p1/BOOT0002.BIN` = `BOOT_GOLDEN.BIN` | `8d42d1a58b62c8ca95501bf486fbb45d` | **兜底 golden**，与板外副本一致 |
| `p1/BOOT0001.BIN` | `bb3402ea4efae7e1062a65632fe684b4` | 已知 10/10 能启动 |
| `p1/BOOT0007.BIN` | `2d7799a32bbf3a01a6e61664b6241198` | J1 镜像（能起但掉网） |
| `p1/BOOT0006.BIN` | —— | **已删除，保持删除** |

`CSU_MULTI_BOOT = 0`，`READY=yes`，PL `operating`。
离板副本：`/home/build/wdt_patch/images/BOOT_SECMMIO.BIN`（= `BOOT.BIN`）、
`/home/build/BOOT_WORKING_REF.BIN`（同一份的保护副本）。

⚠️ 本轮曾覆盖过 `atf_secmmio/.../bl31.elf`（当前 `BOOT.BIN` 所用的那份 BL31 的 ELF），
**没有备份、已丢失**。`BOOT.BIN` 本身完好且有离板副本，所以不影响运行与演示，
但**要改 BL31 就得先解决"重建出来的镜像与在跑的这份不一致"这个问题**。
