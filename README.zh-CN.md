[English](README.md) · **中文**

# pqc-crypto-module

一台跑在 Xilinx Zynq UltraScale+ **XCZU3EG** 可编程逻辑里的后量子 + SM 系列
**硬件密码模块原型**。密码核、噪声源、密钥仓与访问边界全部位于 FPGA 逻辑之中；
ARM 核上的 Linux 只是发命令的那一侧。

本仓库里的每一个数字都来自**真实设备**，不是仿真。

> **状态：研究原型。** 未认证、未加固、不可用于生产。见
> [状态与局限](#状态与局限)——差距是列出来的，不是藏起来的。

---

## 能做什么

| | |
|---|---|
| **后量子 KEM** | ML-KEM-512 / 768 / 1024（FIPS 203），KeyGen / Encaps / Decaps 全流程在 RTL 中实现——**在真硅上**与 NIST ACVP 向量逐字节一致 |
| **后量子签名** | ML-DSA-44 / 65 / 87（FIPS 204），KeyGen / Sign / Verify 在 RTL 中实现，一份位流覆盖三个参数集——**在真硅上**与 ACVP 向量逐字节一致 |
| **对称与 SM 系列** | AES-128/256、SM4、SM3——对照 FIPS 197、GB/T 32907、GB/T 32905 校验 |
| **真随机源** | 8 环形振荡器熵源，SP 800-90B 健康测试（RCT/APT），SHA-3 海绵调节。实测最小熵 **H = 0.871 bit/sample** |
| **硬件密钥仓** | 对称密钥经总线进入，只能沿一条通往密码核的专用线离开。**RTL 中不存在读出路径** |
| **安全边界** | AXI 防火墙按 `AxPROT[1]` 门控，已在板上双向证明：安全世界读得到 `SECURE_ONLY=1` 的核，普通世界在总线上被拒 |
| **标准前端** | SDF 风格（GM/T 0018）C 库与 PKCS#11 v3.2 模块，应用永远不必直接面对寄存器 |

占用**半块器件**：35,659 LUT（50.5 %）、140 DSP、15.5 BRAM，
75 MHz 下 WNS +3.504 ns。

## 架构

```
   ┌──────────────────────── PS · Cortex-A53 ────────────────────────┐
   │  application ──▶ libsdfe (SDF-style)  ──▶ pqchsm_fpgad          │
   │                                              │                  │
   │  ······················· normal world ·······│················  │
   │                                       /dev/secmmio  →  EL3 SiP  │
   └──────────────────────────────────────────────│──────────────────┘
                       M_AXI_HPM0_LPD  (AXI4, AxPROT[1] = security bit)
                                                  ▼
   ┌──────────────────────── PL · FPGA fabric ───────────────────────┐
   │              axi4lite_xbar  (full decode, one address per reg)  │
   │   ┌──────┬──────────┬──────────┬──────────┬─────────┬────────┐  │
   │   │ slot0│  slot1   │  slot2   │  slot3   │  slot4  │ slot5  │  │
   │   │ trng │key_vault │   sym    │  mlkem   │ canary  │  fan   │  │
   │   │      │          │AES/SM4/  │ 512/768/ │ same as │ observe│  │
   │   │      │          │   SM3    │   1024   │ slot 1  │  only  │  │
   │   │      SECURE_ONLY=1 (default build)              │ =0     │  │
   │   └──────┴────┬─────┴────▲─────┴──────────┴─────────┴────────┘  │
   │               └──────────┘  use_key: private wire, not the bus  │
   │      every slot sits behind axi4lite_firewall (AxPROT gate)     │
   └─────────────────────────────────────────────────────────────────┘
```

每个 slot 占 64 KB，位于 `0x8000_0000 + slot × 0x1_0000`。

**默认位流里四个功能从机全是 `SECURE_ONLY=1`** —— 普通世界一个都够不着，整套 KAT
由安全世界经 BL31 SiP 驱动。slot 4 是密钥仓的第二个实例，在这个配置下充当同款对照：
它证明"被拒"是门控干的，不是核坏了。风扇（slot 5）保持 0：它不在密码边界里，也不该在。

另有一份开发用配置——`PQC_DEV_OPEN=1` 把功能从机设成 `SECURE_ONLY=0`，让 Linux
可以直接驱动，产物名为 `zu3eg_hsm_dev.bit`。同一份 RTL，只差一个参数。

> ⚠️ **被拒的访问读回 0，不报错。** 防火墙与地址译码都是 RAZ/WI，所以用户态程序
> 不可能用一个错地址把板子搞挂——这是有意的，并已在真硅上验过（36,000 次被拒访问，
> 板子照常）。代价是敲错地址是静默的：判断是否接上，请读 `VERSION`（每个核都是
> `0x0001_0000`），而不是等一个错误码。细节见
> [docs/REGISTERS.zh-CN.md](docs/REGISTERS.zh-CN.md)。

完整细节：[docs/ARCHITECTURE.zh-CN.md](docs/ARCHITECTURE.zh-CN.md) ·
寄存器级契约：[docs/REGISTERS.zh-CN.md](docs/REGISTERS.zh-CN.md)。

## 快速开始

本节所有内容都不需要板子。

```bash
git clone https://github.com/per1x/pqc-crypto-module
cd pqc-crypto-module

python3 -m venv .venv-rtl && ./.venv-rtl/bin/pip install cocotb
brew install icarus-verilog verilator      # or your distro's packages

./tools/rtl_sim.sh          # 200 cocotb tests against the RTL
./tools/rtl_lint.sh         # Verilator -Wall + Icarus, 70 modules, zero warnings
./tools/rtl_synth_check.sh  # Yosys synthesisability
```

构建主机软件及其 PKCS#11 模块（CMake ≥ 3.20、OpenSSL 3、liboqs）：

```bash
./tools/fetch_vectors.sh && cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

Bitstream（Vivado 2020.1，约 35 分钟，目标器件 `xazu3eg-sfvc784-1-i`）：

```bash
vivado -mode batch -source hardware/syn/impl_bitstream.tcl
```

### 像调用密码机一样调用它

`service/` 给应用提供的是一台真实密码机所暴露的接口。演示程序**不链接任何密码
算法库**（它链的 OpenSSL 只做远程口的 TLS 传输），因此它打印出的任何正确结果
都只可能来自 FPGA。

从另一台机器一条命令跑完整演示（远程口是 **mTLS**）：

```bash
./demo/remote/run.sh                           # 交互问地址；仓库自带演示凭据，零配置
./tools/demo_remote.sh <板子IP> --provision    # 想用自己的 PKI 就走这条（只需一次）
```

```c
#include "sdfe.h"

SDFE_HANDLE dev, ses;
uint8_t ek[1600], ct[1600], ss[32];
uint32_t ek_len = sizeof ek, ct_len = sizeof ct, ss_len = sizeof ss, kh;

SDFE_OpenDevice(&dev);
SDFE_OpenSession(dev, &ses);

SDFE_GenerateRandom(ses, 32, ss);                    /* 环形振荡器 TRNG       */
SDFE_GenerateKeyPair_MLKEM(ses, SDFE_MLKEM_768,      /* dk 不会到达这里       */
                           ek, &ek_len, &kh);
SDFE_Encapsulate_MLKEM(ses, SDFE_MLKEM_768, ek, ek_len,
                       ss, &ss_len, ct, &ct_len);
SDFE_Decapsulate_MLKEM(ses, kh, ct, ct_len, ss, &ss_len);   /* 按句柄 */

SDFE_ImportKey(ses, /*slot*/ 3, key, 16);            /* 写入密钥仓——         */
SDFE_Encrypt(ses, SDFE_ALG_SM4, 3, pt, out);         /* 此后不可读出          */
```

构建并运行：`make -C service && ./service/sdf_demo`。
接口参考：[docs/API.zh-CN.md](docs/API.zh-CN.md)。

## 证据

在 [docs/SECURITY.zh-CN.md](docs/SECURITY.zh-CN.md) 所述的配置下，于设备上实测。

| 检查项 | 结果 |
|---|---|
| ML-KEM 512/768/1024 对照 NIST ACVP，真硅上 | 20 / 20 逐字节一致 |
| 板上自检（对称、SM、边界、AxPROT、TRNG） | 24 / 24 |
| 密钥仓反证——两个从机各扫 256 字节 | 密钥的字出现 **0** 次；密文正确 |
| AxPROT 门控，双向（测于开发位流，那份带一个 `SECURE_ONLY=0` 的对照核） | EL3 读 `SECURE_ONLY=1` 得到 `0x0001_0000`；EL1-NS 读同一地址被拒；同一个 EL1-NS 能读到 `SECURE_ONLY=0` 的核 |
| TRNG 最小熵，1,048,576 个调节前样本 | H = 0.871234 bit/sample → RCT 47、APT 672 |
| Decaps 时序，有效密文 vs 隐式拒绝，各 200 次 | 中位数差异 0.000 % |
| ML-KEM-512 吞吐 @ 75 MHz | 924 / 1339 / 1018 ops/s（KeyGen / Encaps / Decaps） |
| cocotb 回归 · Verilator lint · Yosys | 200 项测试 · 70 个模块，0 条告警 · 全部可综合 |

方法与原始日志见 [docs/TESTING.zh-CN.md](docs/TESTING.zh-CN.md)；板上抓取的输出
原样保存在 [board/logs/](board/logs/) 下。

## 状态与局限

关于本项目**不**做什么的准确陈述。

- **RTL 里仍留着一条私钥导出路径。** 守护进程走的是 `MODE.DK_TO_SLOT`：`dk` 留在
  片内金库、按句柄解封装，正常路径上私钥一个字节都不过总线。但"不进金库"那条路
  仍在，因为 ACVP 验证必须把私钥读出来核对。交付形态下它由一次性闩锁关掉 ——
  是闩锁不是熔丝，断一次电就又开了。
- **root 仍然能驱动硬件。** EL3 SiP 暴露的是白名单内的 MMIO 读写，操作序列在普通
  世界里拼装。普通世界*读*不到密钥材料；但它仍然能装载并使用密钥仓槽位。要堵上
  这一点，需要一个以操作为粒度的 SiP。
- **密钥派生根默认仍是桩，但设备绑定已经做出来了。** `src/crypto/kdr.c` 默认是固定
  常量；设 `PQCHSM_KDR=device-dna` 可切到绑定这颗芯片 Device DNA 的派生根，板上与
  远端主机两条取值路都在真硬件上跑通了。默认不开是因为换根会让既有 keystore 打不开，
  且症状与被篡改一模一样。⚠️ **DNA 不是秘密**（有 JTAG 就能读到），它给的是防克隆，
  不是机密性。
- **没有 SM2 核。** SM4 与 SM3 有；SM2 非对称算法没有。
- **这是一个安全原型，不是一个快的原型。** 吞吐数字包含逐字节的软件 AXI 流量，
  公布它们是为了给出各参数集之间 1 : 1.5 : 2.1 的比例，而不是作为性能主张。
- **功耗与电磁侧信道是有意排除在范围之外的。** 常数时间方面的工作只覆盖时序。
  没有侧信道测试台就无法验证掩码，而交付未经验证的掩码比不做掩码更糟。
- **没有加密启动。** BBRAM 受阻于板级硬件（`VCC_BATT` 没接电池）；PUF 黑钥在
  BootROM 碰到 PUF 之前就被拒收。**RSA 认证启动倒是通的**（零 eFUSE，上板验过），
  但它不是信任根：`bh_auth_enable` 的语义就是跳过 eFUSE 里的 PPK 摘要校验。
  防替换的信任根只有烧 eFUSE 才有，而 eFUSE 是本项目的永久红线。证据与完整论证见
  [docs/SECURITY.zh-CN.md](docs/SECURITY.zh-CN.md)。
- **PS 侧的 XMPU/XPPU 覆盖不到 PL 窗口。** UG1085 对此有定论：没有任何 PS 保护单元
  覆盖 `0x8000_0000`。PL 里的防火墙是这条路径上唯一的执行点——这也正是地址译码
  一一对应、不设镜像的原因。
- **DDR 侧的 XMPU 挡的是"读"，不是"用"。** XMPU 本身不拦截事务，它只打毒标记、
  由终点 gate（XAPP1320 v3.0 p.10）。BL31 开机时把它配好，于是普通世界读 OP-TEE
  安全内存就是 Bus error，没有任何手工步骤。它挡住的仅此一条 —— root 仍然能命令
  硬件做运算，因为 SiP 不校验调用来源。

## 仓库结构

```
hardware/rtl/       Verilog-2001 crypto cores: mlkem/ mldsa/ keccak/ sym/ trng/ bus/ board/
hardware/platform/  Non-crypto fabric logic (fan control) — same bitstream, no shared signals
hardware/tb/        cocotb testbenches, simulation tops, lint-only vendor stubs
hardware/model/     Python reference model, independent oracles, vector export
hardware/syn/       Vivado out-of-context synthesis and the RTL-to-bitstream flow
service/            SDF-style client library, daemon, and a hardware-only demo
boot/atf/           BL31 patches: the EL3 SiP that gives the secure world a path to the PL
board/              On-board programs, harness, kernel modules, and raw result logs
include/ src/ cli/  Host software: keystore, slots, backup, audit, PKCS#11 front end
tee/                OP-TEE trusted application (separate line of work)
tests/              Host-software unit, integration, KAT and fuzz targets
tools/              Regression scripts, SP 800-90B estimators, static analysers
docs/               Architecture, API, security model, testing, register maps
```

## 文档

| | |
|---|---|
| [ARCHITECTURE.zh-CN.md](docs/ARCHITECTURE.zh-CN.md) | 分层、地址映射、时钟、密钥层级、硬件接缝 |
| [API.zh-CN.md](docs/API.zh-CN.md) | SDF 风格接口与 PKCS#11 v3.2 前端 |
| [SECURITY.zh-CN.md](docs/SECURITY.zh-CN.md) | 信任边界、威胁模型、什么已证明、什么没有 |
| [REGISTERS.zh-CN.md](docs/REGISTERS.zh-CN.md) | 每个 AXI 从机的寄存器契约 |
| [TESTING.zh-CN.md](docs/TESTING.zh-CN.md) | 测什么、用什么手段测、如何复现 |
| [USAGE.zh-CN.md](docs/USAGE.zh-CN.md) | 构建、运行、部署，以及如何驱动板子 |
| [reference/](docs/reference/) | 安全策略草稿、离线部署、常量时间审计、移植计划 |

英文版本以去掉 `.zh-CN` 后缀的同名文件并列存放。`./tools/pdf/build-pdf.sh` 可以把
架构、API、寄存器、安全与测试这五份从同一批 Markdown 重出为一份合订 PDF。

## 参与贡献

见 [CONTRIBUTING.md](CONTRIBUTING.md)；要动板子的人请先读
[不可逆操作红线](docs/SECURITY.zh-CN.md#-不可逆操作红线)。报告漏洞前请先读
[SECURITY.md](SECURITY.md)——但请注意这是一个原型，上面列出的已知差距已经是
已知的。

## 许可证

[Apache-2.0](LICENSE)——选它而不选 MIT，是因为它有明确的专利授权与专利反制条款；
专利在这个领域是真实的风险敞口，而 MIT 对此只字未提。

`third_party/pkcs11-v3.2/` 收录了三个 OASIS 头文件，按其各自的条款原样引入。
