# 移植到 Zynq UltraScale+ MPSoC

本文是一份**设计与评估文档**：它给出把本模块从纯软件原型移植到 Zynq UltraScale+
MPSoC 的阶段划分与依赖关系，并逐条说明当前靠软件假设成立的安全性质，在这颗芯片上
分别由哪个硬件机制强制。

本文不包含实现。文中所有 RTL、固件、eFUSE 操作均未执行过，硬件相关的结论应在实际
设备上复核后才可引用。

## 0. 目标硬件

| 项 | 取值 |
|---|---|
| 器件 | XCZU3EG-1SFVC784I（Zynq UltraScale+ MPSoC，EG 系列） |
| 板卡 | ALINX AXU3EGB |
| APU | 4× Cortex-A53 |
| RPU | 2× Cortex-R5F（可选锁步 / 分离模式） |
| PL | 约 154K 逻辑单元、约 70.6K LUT、360 DSP、7.6 Mb BRAM |
| PS 内存 | 4 GB DDR4 |
| PL 内存 | 1 GB DDR4（独立于 PS DDR） |
| 非易失存储 | 8 GB eMMC、256 Mb QSPI Flash |
| 启动模式 | JTAG / QSPI32 / SD / eMMC（板上拨码选择） |
| 网络 | 2× 千兆以太网（PS GEM 一路、PL 一路）、M.2（PCIe x1，走 PS-GTR） |
| 其它接口 | DP、4× USB3.0、2× UART、SD、2× 40pin 扩展、2× CAN、2× RS485、MIPI、JTAG |
| 板载器件 | RTC、LM75 温度传感器、24LC04 EEPROM |
| 配套工具链 | Vivado / PetaLinux 2020.x |

PL 规模决定了一个现实约束：约 70.6K LUT 与 360 个 DSP 足以容纳本仓库现有的算术核
（NTT、Keccak、采样与打包）加上总线接口，但**不足以**放下一个完整的、高并行度的
ML-KEM 与 ML-DSA 数据通路加上 DMA 与安全外围。资源预算应在做任何完整算法核的计划
之前先实测（out-of-context 综合已有脚本，见 `hardware/syn/`）。

### 工具链宿主

Vivado 与 PetaLinux 2020.x 仅支持 x86_64 Linux 宿主（官方列表为 Ubuntu 18.04/20.04、
CentOS/RHEL 7/8），不支持 macOS，也不支持 aarch64 宿主。因此工具链需要一台独立的
x86_64 Linux 机器或虚拟机；本仓库现有的 aarch64 容器回归（`tools/aarch64_test.sh`）
覆盖的是**目标侧软件构建**，不能替代综合环境。

## 1. 现状与硬件的对应关系

本仓库在无硬件阶段做的每一处抽象，都对应一个此处需要落地的硬件机制：

| 软件阶段的抽象 | 位置 | 对应的硬件机制 |
|---|---|---|
| `pqc_kdr_provider_t`（根密钥 provider） | `include/pqchsm/kdr.h` | PUF / BBRAM / eFUSE |
| `accel_transport_t`（四种 transport） | `include/pqchsm/accel.h` | PL 加速器 + AXI |
| `pqc_backend_t`（算法后端 vtable） | `include/pqchsm/pqc.h` | PL 算法核 |
| 制造模式开关 `g_mfg_blown` | `src/backup/inject.c:215` | eFUSE 用户位 |
| `hsm_slot_zeroize_forced()` | `include/pqchsm/slot.h:203` | CSU tamper 响应 |
| `pqc_kdr_is_hardware_backed()` | `include/pqchsm/kdr.h` | 上电自检的判据 |
| `trng_health` 噪声源健康检测 | `hardware/rtl/trng/trng_health.v` | PL 环形振荡器或 CSU TRNG |
| TLV 协议的传输层可替换 | `include/pqchsm/proto.h` | UART / 千兆网 / M.2 |

因此上板不是重写，而是**逐个接缝换实现**。本文其余部分给出换的顺序，以及每一次替换
把哪条"软件假设"变成"硬件强制"。

---

# A. 真实硬件解锁的工作

## A.0 依赖关系

```
  P0 板卡 bring-up ──┬── P1 软件跑到 A53（真机功能对齐 + 基准）
                     │        └── P8 对外 API 走真实千兆网 / M.2
                     ├── P2 PL 加速器上板（mmap transport 首次有真目标）
                     │        └── P9 PL 里的完整 PQC 加速器（大工程）
                     ├── P3 安全启动链（**JTAG 模式验证，不烧 eFUSE**）
                     │        └── P7 eFUSE 烧写（不可逆，排最后）
                     └── P4 TrustZone：ATF + OP-TEE + XMPU/XPPU
                              ├── P5 KDR 落到 PUF / BBRAM
                              │        └── P6 密钥操作搬进安全世界
                              └── P10 tamper 与环境监测
```

排序原则有三条：**先能跑、再能测、最后才能防**；**每一步都要有可执行的反证**；
**不可逆的步骤一律排在可逆步骤全部跑通之后**。

## P0 板卡 bring-up

**依赖**：无（一切的前置）。

内容：安装 Vivado / PetaLinux 2020.x；以 ALINX 提供的板级配置（PS 预设、DDR4 型号、
MIO 分配）建立 base platform；生成 XSA 与 BOOT.BIN；SD 卡启动 PetaLinux。

**验收**：从 SD 启动进入 Linux 控制台；`/proc/cpuinfo` 显示 4 个 A53 核（手册中
"双核"的措辞与器件实际不符，此处以实测为准）；`free` 显示 4 GB PS DDR；两个网口
`ip link` 均可 up 且能 ping 通；eMMC 与 QSPI 可读写；LM75 与 24LC04 在 I²C 总线上
可见。

**风险**：2020.x 与较新宿主发行版的 glibc/Python 版本冲突是这一步最常见的阻塞点，
与本项目无关但会消耗时间。

## P1 现有软件跑到 A53

**依赖**：P0。**与 P2、P3 可并行。**

这一步已经在无硬件阶段预演过：`tools/aarch64_test.sh` 在 aarch64 Linux 容器里从零
构建并跑完整回归。板上要做的是把同一条路径在真实 A53 上再跑一遍，差异只在
liboqs/OpenSSL 的构建来源与 CPU 特性。

**产出**：

- `ctest` 全套结果（当前 macOS 与 aarch64 容器上均全绿，真机结果需另行记录）
- `build/pqchsm-bench`：ML-KEM/ML-DSA 各参数集在 A53 上的算法级基线
- `build/pqchsm-prim-bench`：单次原语代价

这是本项目**第一份真实硬件性能数字**。此前所有数字来自开发机或仿真周期计数，两者都
不能代表目标平台：A53 没有开发机上那套 NEON 优化路径的同等实现，绝对值会明显不同。

**验收**：`ctest` 全绿；两份基准产出可复现的数字并记入 `docs/testing.zh-CN.md`。

**注意**：这一步不需要 PL、不需要 TrustZone、不需要安全启动，因此优先级仅次于 P0。
它也是后续所有性能对比的分母。

## P2 PL 加速器上板

**依赖**：P0。

把 `hardware/rtl/bus/pqc_accel_axi` 综合进 base platform，挂在 PS 的 AXI 主口上。
ZynqMP 的地址分配与 Zynq-7000 不同：`M_AXI_HPM0_FPD` 落在 `0xA000_0000`、
`M_AXI_HPM1_FPD` 落在 `0xB000_0000`、`M_AXI_HPM0_LPD` 落在 `0x8000_0000`。
`src/hal/accel_mmap.c:9` 的注释里提到的 `0x4000_0000` 是 Zynq-7000 的取值，上板时
需要按实际地址分配更新。

`accel_transport_mmap()` 至此首次拥有真实目标：

```bash
cmake -S . -B build -DCMAKE_C_FLAGS="-DPQCHSM_ACCEL_MMAP_BASE=0xA0000000 \
                                     -DPQCHSM_ACCEL_MMAP_BUF=0xA0010000"
```

**产出与验收**：`tests/unit/test_accel.c` 现有的逐字节一致性断言，含义从"软件桩对仿真
RTL"升级为"软件桩对真硅"。这是同一份测试代码在硬件上的直接复用，不需要新写测试。
`accel_axi_last_cycles()` 给出的软件视角时延可与真机 mmap 路径对照。

**诚实预期**：单蝶形 NTT 核在 100 MHz 下每次变换 1153 周期，约 11.5 µs；A53 上同一
变换的软件耗时需实测。**加速比小于 1 是完全可能的结果**，那也是有效结论——现有 RTL
的目的是把接口与验证方法学做对，不是竞速。

**过渡说明**：`/dev/mem` 只适合 bring-up 阶段。它绕过一切内核访问控制，与 P4 之后的
安全模型直接冲突；正式路径应改为 UIO 或专用驱动，并在 P6 之后只在安全世界映射。

## P3 安全启动链（在 JTAG 模式下验证）

**依赖**：P0。**本阶段不烧任何 eFUSE。**

ZynqMP 的启动链是 BootROM → FSBL → PMU 固件 / ATF(BL31) → U-Boot → Linux。
CSU 中的 BootROM 用 RSA-4096 验签（散列为 SHA3-384）、用 AES-256-GCM 解密启动镜像。
信任根是烧在 eFUSE 里的主公钥散列（PPK hash）。

本阶段的做法是把不可逆的部分全部推后：

1. AES 密钥先放 **BBRAM**（可擦写、可重写），不放 eFUSE；
2. 不烧 PPK hash、不置 `RSA_EN`，因此 BootROM 此时**不强制**验签；
3. 用 `bootgen` 产出签名并加密的镜像，通过 JTAG 加载，验证 FSBL 能正确解密与自校验；
4. 用 `xsct` / JTAG 逐级观察启动，确认每一级的交接正常。

**验收（反证优先）**：修改已签名镜像中的任意一个字节，加载应当失败；把 BBRAM 密钥
擦除，加密镜像应当无法启动。**没有跑通这两条反证，就不能进入 P7。**

**风险**：本阶段的所有操作都可回退——BBRAM 可擦写，eFUSE 未动，JTAG 可用。这正是
把它安排在此处的原因。

## P4 TrustZone：ATF + OP-TEE + XMPU/XPPU

**依赖**：P0（功能上）；建议在 P3 之后，因为未经认证的安全世界固件不构成信任根。

内容：

- ATF（BL31）运行在 EL3；OP-TEE OS 作为 BL32 运行在 S-EL1；Linux 作为普通世界
  运行在 NS-EL1。PetaLinux 2020.x 提供这一组合。
- 从 4 GB PS DDR 中划出一段安全 DDR（典型量级 64–256 MB），用 **XMPU** 设定为仅安全
  访问；普通世界对该区间的读写被硬件拦截。
- 用 **XPPU** 限制普通世界对 LPD 外设寄存器空间的访问，尤其是 CSU 与 eFUSE 控制器。
- 普通世界经 GlobalPlatform TEE Client API（`libteec` + `tee-supplicant`）调用 TA。

**验收（反证优先）**：普通世界读安全 DDR 区间应触发 XMPU 违例（并可从中断/错误寄存器
观察到），而**不是**读到数据；普通世界直接访问被 XPPU 保护的寄存器应失败。这两条是
"隔离边界真的存在"的唯一证据，缺一不可。

**上游先例**：OP-TEE 自带一个实现 PKCS#11 的 Cryptoki TA，其结构与本项目
`src/p11/p11_module.c` 作为普通世界客户端库、密钥操作在 TA 内完成的划分一致。这条
路径不是本项目独有的设计。

## P5 KDR 落到 PUF / BBRAM

**依赖**：P4（根密钥必须在安全世界内使用，否则派生结果会经过普通世界内存）。

见 B.2。这是把"软件模拟的安全边界"变成真实边界最关键的一步。

## P6 密钥操作搬进安全世界

**依赖**：P5。见 B.4 的分层方案。

## P7 eFUSE 烧写（不可逆）

**依赖**：P3 与 P5 的全部反证均已在 BBRAM / JTAG 模式下跑通。

见第 C 节。**本阶段之前的每一步都可回退，本阶段之后一步都退不回来。**

## P8 对外 API 走真实千兆网 / M.2

**依赖**：P1。

`cli/pqchsmd.c` 的 TCP 传输层直接可用。`include/pqchsm/proto.h` 已经写明该协议
**不做认证与加密**，走网络必须外套 TLS——这条约束在真机上从纸面变成实际部署要求。

板上有两路千兆网：PS 侧 GEM 与 PL 侧各一路。一个合理的划分是 PS 网口做管理面、
PL 网口或 M.2 做数据面，两者在 XMPU/XPPU 层面拥有不同的可达范围。M.2 的 PCIe x1
走 PS-GTR，其收益需在实测吞吐后判断——本模块的单次操作数据量（公钥、签名、密文）
在千兆网上不构成瓶颈，PCIe 的价值主要在高并发批量场景。

## P9 PL 里的完整 PQC 加速器

**依赖**：P2 与 P1 的实测数字。

现有 RTL 覆盖算术核（NTT、基乘、压缩、采样、打包、Keccak 置换）与总线接口，**不是**
完整的 ML-KEM 或 ML-DSA。做完整数据通路是一项独立的大工程，且在 70.6K LUT 的器件上
需要先做资源预算。

**优先级最低的理由**：在 P1 与 P2 给出真机数字之前，无法判断这项工作的收益。若 A53
上的 liboqs 已经满足目标吞吐，则这项工作的价值在于"密钥运算不经过通用 CPU"这一
**安全**属性，而非性能——那样的话它的设计目标与优先级都不同，应当重新评估。

## P10 tamper 与环境监测

**依赖**：P3（tamper 响应需要可信固件）、P4。

见 B.3.6。

---

# B. 把软件模拟的边界映射成真实硅片边界

## B.1 这颗芯片提供的安全原语

| 原语 | 位置 | 用途 |
|---|---|---|
| CSU（Configuration Security Unit） | PS | BootROM、AES-256-GCM、RSA-4096、SHA3-384、PUF、TRNG、eFUSE 控制器 |
| eFUSE | PS | AES 密钥、PPK0/PPK1 散列、SPK ID、用户位、安全控制位。**一次性、不可逆** |
| BBRAM | PS | 256 位 AES 密钥，电池保持，**可擦写重写** |
| PUF | CSU | 由器件物理特性生成设备唯一密钥，用于"黑钥"加密；需要注册并保存 helper data |
| TrustZone | APU | EL3 / S-EL1 / NS-EL1 分层，AXI 事务携带 `AxPROT[1]`（NS 位） |
| XMPU | PS | 按地址区间与 master 做读写与安全属性检查（DDR、OCM、FPD 各有实例） |
| XPPU | PS | 按 master 保护 LPD 外设寄存器空间 |
| SYSMON | PS/PL | 片上电压与温度监测，可作为环境失效与 tamper 触发源 |
| tamper 响应 | CSU/PMU | 触发源包括 JTAG 活动、SYSMON 越界、外部 MIO 引脚；响应可配置为擦除 BBRAM、安全锁定、系统复位 |

区域数量、寄存器细节与响应可配项以 UG1085（ZynqMP TRM）与 UG1137 为准，本文不复述。

**一处需要澄清的说法**：Vivado 中的 "AXI Firewall" IP 处理的是协议违规与总线挂死，
**不是**访问控制部件。按 master 与安全属性过滤访问，靠的是 PS 侧的 XMPU/XPPU，
以及 PL 侧自行对 `AxPROT[1]` 与 master ID 作判决的逻辑。本文后续提到"只允许安全
master 访问加速器"时，指的是后者。

**一处需要注意的信任假设**：PL master 发起事务时的 `AxPROT[1]` 由 PL 逻辑自己给出。
因此"PL 是可信的"这一前提，来自比特流本身经过认证加载（P3），而不是来自总线本身；
下游仍应由 XMPU/XPPU 按地址区间复核。

## B.2 逐条映射

| 现在的软件假设 | 位置 | 硅片上的强制手段 | 改造点 |
|---|---|---|---|
| 根密钥是源码里的常量 | `src/crypto/kdr.c:11` `KDR_STUB` | PUF 黑钥 / BBRAM / eFUSE，根密钥不出 CSU | 新增 `kdr_zynqmp` provider，`hardware_backed = 1` |
| "接口里没有读出函数"所以读不到根密钥 | `include/pqchsm/kdr.h`，由 `tools/check_no_readback.py` 结构性回归保证 | 地址译码上物理不存在读回路径，且 CSU 寄存器被 XPPU 挡在普通世界之外 | 接口不改，改的是这句话成立的**理由** |
| KEK 明文存在于进程内存 | `src/store/wrap.c`、`pqc_kek_derive()` | 安全 DDR，XMPU 拦截普通世界访问 | wrap/unwrap 移入 TA，KEK 不出 S-EL1 |
| 制造模式开关是进程内变量 | `src/backup/inject.c:215` `g_mfg_blown` | eFUSE 用户位，一次性熔断 | 读真实 eFUSE；**排在最后** |
| 强制清零靠调用方主动调用 | `include/pqchsm/slot.h:203` `hsm_slot_zeroize_forced()` | CSU tamper 响应链 | PMU 固件触发 → TA → 调用现有函数 |
| 模块映像使用前未经校验 | 无（安全策略 §13.3 记录为差距） | BootROM RSA-4096 + SHA3-384 + AES-256-GCM | `bootgen`；Linux 侧另加 dm-verity |
| 安全边界是进程地址空间 | 全局（安全策略 §13.5） | TrustZone + XMPU/XPPU | 见 B.4 分层 |
| 加速器寄存器谁映射谁就能驱动 | `src/hal/accel_mmap.c` 走 `/dev/mem` | XMPU/XPPU 只放行安全 master；PL 侧判 `AxPROT[1]` | 去掉 `/dev/mem`，改由 TA 映射 |
| 随机数来自宿主操作系统 | `src/util/util.c:30` `pqc_random_bytes()`，以及散落各处的 `RAND_bytes` | CSU TRNG，或 PL 环形振荡器配 `trng_health` | 先把散落调用收敛到 `pqc_random_bytes()`，再换 provider |
| 审计日志可被整体重写，靠外部锚点发现 | `src/audit/`、`include/pqchsm/anchor.h` | eFUSE 用户位作单调计数器，或 eMMC RPMB | 锚点机制不变，增加设备内单调计数 |
| 协议无认证无加密 | `include/pqchsm/proto.h` | 无对应硬件机制 | 仍需 TLS，或把链路限制在安全世界的 UART |

## B.3 展开

### B.3.1 信任根与安全启动

上电后 CSU 中的 BootROM 是唯一的不可变代码。它用烧在 eFUSE 中的 PPK 散列校验启动
镜像的 RSA-4096 签名，并用 eFUSE 或 BBRAM 中的 AES-256 密钥解密镜像。置位 `RSA_EN`
后，BootROM 拒绝执行任何未通过验签的镜像。

这条链把"模块映像未经校验"（安全策略 §13.3）从软件差距变成硬件强制：只有持有私钥
的一方签出的固件能在这块板上运行。链条继续向上：FSBL 校验 PMU 固件与 ATF，ATF 校验
OP-TEE，U-Boot 校验内核（FIT 镜像签名），Linux 用 dm-verity 校验根文件系统。**每一级
都必须校验下一级，链条断在哪里，信任就止于哪里。**

SPK ID 与 eFUSE 中的撤销位提供密钥撤销能力：签名密钥泄露后可以让旧签名失效。这同样
是不可逆操作。

### B.3.2 密钥存储：KDR 的三条落地路径

`include/pqchsm/kdr.h` 已经把 provider 结构定死，落地时上层一行不改。三条路径：

| provider | 根密钥来源 | 可逆性 | 评价 |
|---|---|---|---|
| `bbram` | BBRAM 中的 256 位密钥 | 可擦写重写 | **首选起点**：语义与 eFUSE 相同但可回退，适合把整条链跑通 |
| `puf` | PUF 生成的设备唯一密钥 | 注册可重做 | **首选终点**：密钥不以任何形式静态存储，只存 helper data |
| `efuse` | eFUSE 中的 256 位密钥 | **不可逆** | 仅在前两者验证通过后考虑 |

PUF 路径的工作方式是"黑钥"：设备主密钥用 PUF 派生的 KEK 加密后存放（在 eFUSE 或
启动头中），使用时由 CSU 内部用重新生成的 PUF 密钥解密，明文主密钥**不进 DRAM**。
PUF 注册产生 helper data（syndrome、CHASH、AUX），需妥善保存；helper data 丢失等于
主密钥丢失。

改造点在于 `derive()` 的执行位置。`pqc_kdr_derive()` 的语义是"给定 label 与 salt
返回子密钥"，硬件 provider 的实现应当是一次到 TA 的调用：**根密钥与 KDF 计算都在
安全世界内完成，普通世界只拿到派生结果**。若把根密钥读进普通世界再做 KDF，边界就
仍在软件里。

`pqc_kdr_is_hardware_backed()` 与上电自检的关系已经写在头文件里：`hardware_backed`
为 0 时上层可拒绝进入生产模式。这一判据在无硬件阶段就已成立，落地后自动生效。
`tools/check_no_readback.py` 的结构性检查同样保留——它保证接口层面不会退化，硬件
只是给同一句话加了第二层保证。

### B.3.3 隔离边界

真正的边界由两件事共同构成，缺一不可：

1. **TrustZone 划分执行环境**：密钥材料与密钥操作在 S-EL1，普通世界的应用只能通过
   定义好的 TA 接口请求服务。
2. **XMPU/XPPU 划分地址空间**：安全 DDR 区间、CSU 与 eFUSE 控制器寄存器、PL 加速器
   的寄存器与数据窗口，都只对安全 master 开放。

只做第一件而不做第二件是常见错误：普通世界的驱动如果还能 `mmap` 到加速器寄存器或
安全 DDR，TrustZone 的划分就被绕开了。本项目现有的 `/dev/mem` 路径正是这种情况，
因此 P2 之后必须改掉。

**PL DDR 的特殊性**：板上 1 GB PL DDR 独立于 PS DDR，**不受 PS XMPU 保护**。若加速器
的工作缓冲放在 PL DDR，其访问控制必须在 PL 内部自行实现。最简单的做法是不放敏感中间
结果——但 NTT 的输入输出就是私钥多项式，这个前提不成立。因此使用 PL DDR 前必须先解决
它的访问控制，或者干脆只用 PL 内部 BRAM 作工作缓冲（现有 `pqc_accel_axi` 的数据缓冲
按已实现操作码定尺寸为 4096 位，正是这个方向）。

### B.3.4 密码引擎的分工

| 算法 | 落点 | 说明 |
|---|---|---|
| AES-256-GCM | CSU 硬件引擎 | 密钥库包裹（`src/store/wrap.c`）可改走 CSU，密钥可以是永不出 CSU 的 BBRAM/eFUSE/PUF 密钥 |
| SHA3-384 | CSU 硬件引擎 | 启动镜像散列；本模块自身用的是 SHA3-256，参数不同 |
| RSA-4096 | CSU 硬件引擎 | 仅用于启动验签，本模块不使用 |
| SHA3-256 / KMAC256 | 软件（OpenSSL），或 PL 的 `keccak_f1600` | 审计链与元数据标签 |
| ML-KEM / ML-DSA | 软件（liboqs），可选 PL 加速 | 见 P9 |

CSU 引擎的价值不在速度，而在**密钥可以永不进入 DRAM**：CSU AES 可以直接选用 BBRAM
或 eFUSE 中的密钥作为密钥源。把 `pqc_wrap` / `pqc_unwrap` 改走这条路径，KEK 就从
"安全世界内存里的 32 字节"变成"从不以明文出现在任何可寻址存储中的值"。这是比 XMPU
更强的一层。

代价是 `wrap.h` 中的 blob 格式与 nonce 构造需要与 CSU AES 引擎的接口对齐，且 CSU
引擎的调用只能在安全世界内发出。改造应在 P6 之后进行。

### B.3.5 密钥恢复

`include/pqchsm/backup.h` 的层级已经把两条路分开：KEK 绑定设备（KDR 派生），BEK 不
绑定设备（RMK 派生，Shamir 分片离线保管）。这个划分在硬件上不变——**它本来就是为了
让 KDR 可以是不可导出的硬件密钥而设计的**。

落地时的改造点：

- 备份导出与恢复导入过程中，RMK 与 BEK 应当只在安全世界中存在。当前
  `src/backup/backup.c:86` 在普通进程内存里生成 RMK，落地后应移入 TA。
- Shamir 分片的产出必须离开设备（交由不同保管员），因此分片是**唯一**允许跨越安全
  边界向外的密钥材料。这个例外必须显式记录在安全策略里，并由 TA 接口层面限制为
  只在 SO 会话下、只在备份导出流程中可用。
- `src/backup/shamir.c:114` 的随机系数取自 `RAND_bytes`，落地后应改用边界内熵源。
- 恢复到替换设备时，新设备的 KDR 不同，因此密钥库必须用新设备的 KEK 重新包裹。
  这一行为在软件阶段已经实现并有跨设备负测试（`pqc_kdr_set_test_root()` 用于模拟
  另一台设备），落地后该测试的语义从"模拟"变成"真实"。

### B.3.6 tamper 与环境监测

`include/pqchsm/slot.h:203` 的注释已经写明 `hsm_slot_zeroize_forced()` 对应硬件
tamper 线。落地路径：

- **触发源**：SYSMON 的电压/温度越界（板上另有 LM75 可作独立交叉验证）、JTAG 活动
  检测、外部 MIO 引脚接机箱开关。
- **硬件响应**：CSU 可配置为擦除 BBRAM 密钥、进入安全锁定、系统复位。BBRAM 被擦除
  即意味着密钥库再也解不开——这是最强的一层，且不依赖软件是否还在运行。
- **软件响应**：PMU 固件收到 tamper 事件后通知安全世界，由 TA 调用
  `hsm_slot_zeroize_forced()` 清理槽位并写审计记录。

两层响应的关系要说清楚：**硬件响应是兜底**（软件已经失控时仍然生效），软件响应负责
留下证据。设计时不能只做后者。

**这块板的现实限制**：AXU3EGB 是评估板，JTAG 排针在板、扩展口全部引出、没有防拆
外壳。它可以用来验证 tamper 响应链是否工作，但**不构成 FIPS 140-3 意义上的物理安全
边界**。安全策略 §13.5 记录的这条差距，在这块板上只能部分改善，不能关闭。

## B.4 分层落点：什么进安全世界

把整个模块搬进 TA 与只把根密钥搬进 TA 是两个极端，都不合适。建议分两步：

**第一步（P5/P6）——把密钥材料关进去，把策略留在外面：**

| 组件 | 落点 | 理由 |
|---|---|---|
| KDR provider、KEK 派生 | TA | 根密钥与派生结果都不应出现在普通世界 |
| `pqc_wrap` / `pqc_unwrap` | TA | KEK 不出安全世界 |
| 私钥参与的运算（签名、解封装、密钥生成） | TA | 私钥明文只在 S-EL1 |
| 槽位状态机、会话、ACL、PIN 校验 | 普通世界 | 这些是策略不是秘密；PIN 校验值本就不经 KDR |
| 密钥库文件 I/O | 普通世界 | 文件内容已被 GCM 认证加密 |
| PKCS#11 前端（`src/p11/`） | 普通世界 | 作为 TA 的客户端库 |
| 审计链写入 | 普通世界 | 内容不敏感；完整性靠哈希链与锚点 |

这一步的边界正好落在 `include/pqchsm/pqc.h` 与 `include/pqchsm/wrap.h` 这两个已有
接口上——**这不是巧合，而是这两个接口当初就是按"上层一行不改"的约束设计的**。

**第二步（可选）——把槽位管理器整体搬进 TA：**

更强，但需要 TA 内实现文件访问（经 `tee-supplicant` 回到普通世界，仍需信任其可用性
而非其正确性），且 TA 的堆与栈需要相应调大。是否值得取决于威胁模型中"普通世界内核
被攻破"的权重。安全 DDR 划出 64–256 MB 在 4 GB PS DDR 上不构成压力，因此容量不是
限制因素。

一个必须提前想清楚的问题：PIN 校验若留在普通世界，则内核被攻破的攻击者可以绕过
PIN 直接请求 TA 做签名。若把 PIN 校验也搬进 TA，则 TA 需要维护会话状态。**第一步的
方案抵抗的是"普通世界应用"而非"普通世界内核"**，这一点必须在安全策略里写明，不能
含糊。

## B.5 对安全策略差距清单的影响

`docs/security-policy.zh-CN.md` §13 列出十条阻断送检的差距。移植后的状态：

| # | 差距 | 移植后 |
|---|---|---|
| 1 | 没有设备绑定 | **关闭**（P5：PUF/BBRAM/eFUSE） |
| 2 | 边界内没有熵源 | **关闭**（CSU TRNG 或 PL 噪声源 + `trng_health`） |
| 3 | 没有模块完整性校验 | **关闭**（P3：安全启动 + dm-verity） |
| 4 | 缺少条件自测试 | 不受影响，是软件工作，可在任何阶段补 |
| 5 | 没有物理安全 | **部分改善**：tamper 响应链可用，但评估板不是防拆外壳 |
| 6 | 没有算法证书 | 不受影响，属于送检流程而非技术工作 |
| 7 | 审计日志假定单写者 | 不受影响 |
| 8 | Shamir 分片校验和不带密钥 | 不受影响 |
| 9 | 管理员 PIN 未强制锁定 | 不受影响 |
| 10 | 没有完整算法的硬件实现 | 取决于 P9 是否推进 |

十条中硬件能关闭三条、部分改善一条，其余六条仍是软件或流程工作。**拿到板子不等于
接近送检**——这一点应在任何对外表述中保持准确。

---

# C. 不可逆步骤与变砖风险

## C.1 不可逆操作清单

以下操作**一经执行无法撤销**，且部分会永久改变这块板的可用性：

| 操作 | 后果 | 撤销可能 |
|---|---|---|
| 烧写 eFUSE AES 密钥 | 该密钥永久固定 | 无 |
| 烧写 PPK0/PPK1 散列 | 只有对应私钥签的镜像能启动 | 无（可用第二把 PPK 作备份） |
| 置位 `RSA_EN` | BootROM 强制验签 | 无 |
| 置位 JTAG 禁用位 | **永久失去 JTAG 调试与恢复能力** | 无 |
| 置位加密强制位 | 只能启动加密镜像 | 无 |
| 烧写 SPK 撤销位 | 对应签名密钥永久失效 | 无 |
| 烧写用户 eFUSE 位（制造模式熔断、单调计数） | 位只能由 0 变 1 | 无 |
| eMMC RPMB 密钥写入 | RPMB 密钥一次性写入 | 无 |

BBRAM 密钥**不在**此列——它可擦写重写，这正是它适合作为验证阶段替身的原因。

## C.2 顺序规则

1. **整条链先在 BBRAM + JTAG 模式下跑通**，包括正向验收与反证（P3）。
2. **KDR 先用 `bbram` provider 跑通**全部密钥库、备份、恢复流程（P5），确认换根密钥
   后旧密钥库确实解不开、恢复流程确实可用。
3. **PUF 注册先做一遍并保存 helper data**，确认重新生成的密钥稳定（跨温度、跨上电
   多次验证）。
4. 只有以上全部通过，才考虑烧 eFUSE（P7），且**按依赖顺序逐位烧、每烧一位验证一次**，
   绝不一次性烧完整套安全控制位。
5. **JTAG 禁用位永远最后**，且只在确认不再需要板级调试之后。

## C.3 单板约束

**目前只有一块板。** 在这块板上烧写 eFUSE 安全控制位（尤其是 JTAG 禁用与 `RSA_EN`）
意味着：

- 一旦签名或加密流程有任何配置错误，板子无法启动且无法用 JTAG 恢复——**这就是变砖**；
- 即使一切正确，后续的 P9（PL 加速器开发）也会因失去 JTAG 调试而变得极其困难。

因此本文的建议是明确的：**在只有一块板的情况下，不烧任何 eFUSE 安全控制位。**
P3 到 P6 的全部技术内容都可以用 BBRAM + JTAG 模式验证到位，唯一无法验证的是
"BootROM 强制验签"这一条本身——而验证它需要一块可以牺牲的板。

若确实需要走到 P7，合理做法是准备第二块板：一块保持可调试状态用于开发，另一块作为
"生产形态"板烧写并验证。

## C.4 JTAG 模式下验证整条链的具体做法

在不烧任何 eFUSE 的前提下，以下性质都可以验证：

| 待验证性质 | 验证方法 |
|---|---|
| 镜像签名格式正确 | `bootgen` 产出后用 JTAG 加载，FSBL 自校验通过 |
| 镜像加密正确 | AES 密钥放 BBRAM，加载成功；擦除 BBRAM 后加载失败 |
| 篡改可检出 | 改镜像一个字节，加载失败（反证） |
| ATF/OP-TEE 链正常 | JTAG 观察各级交接，`tee-supplicant` 起来 |
| XMPU 隔离生效 | 普通世界读安全 DDR 触发违例（反证） |
| XPPU 隔离生效 | 普通世界访问受保护寄存器失败（反证） |
| KDR 硬件绑定 | 用 BBRAM provider，换 BBRAM 密钥后旧密钥库解不开（反证） |
| PUF 稳定性 | 多次上电与温度变化下重新生成密钥一致 |
| tamper 响应 | 触发 SYSMON 阈值或 MIO 引脚，观察 BBRAM 被擦除 |

**唯一必须靠烧 eFUSE 才能验证的是 BootROM 的强制验签行为**。它的正确性依赖于 AMD
的 BootROM 实现而非本项目的代码，因此把它推到最后、或者不做，都不影响本项目其余
部分的正确性论证。

---

# D. 本文不涵盖的内容

- 具体的 Vivado 工程配置、约束文件与时序收敛。资源与时序需在实际综合后确定。
- PL 侧完整 ML-KEM/ML-DSA 数据通路的微架构设计（P9 的前置工作）。
- 侧信道防护。TrustZone 与 XMPU 不提供任何针对功耗、电磁或缓存时序侧信道的防护；
  `docs/constant-time.zh-CN.md` 中的分析针对的是软件时序，在 A53 上需重新审视。
- 商用密码算法体系（SM2/SM3/SM4/ZUC）。GM/T 0028 送检需要它们，本模块一个都没有
  实现，与是否上板无关。

## 参考文档

文档编号对应 AMD/Xilinx 2020.x 版本，使用前应核对：UG1085（Zynq UltraScale+ 器件
技术参考手册）、UG1137（MPSoC 软件开发者指南）、UG1283（Bootgen 用户指南）、
DS891（Zynq UltraScale+ MPSoC 数据手册）。板级资料以 ALINX AXU3EGB 随附文档为准。
