[English](README.md) · **中文**

# pqc-crypto-module

一台跑在 Xilinx Zynq UltraScale+ **XCZU3EG** 可编程逻辑里的后量子 + SM 系列
**硬件密码模块原型**。密码核、噪声源、密钥仓与访问边界全部位于 FPGA 逻辑之中；
ARM 核上的 Linux 只是发命令的那一侧。

本仓库里的每一个数字都来自**真实设备**，不是仿真。

> **状态：研究原型。** 未认证、未加固、不可用于生产。见
> [状态与局限](#状态与局限)——差距是列出来的，不是藏起来的。

> ### ⛔ 动板子之前先读：不可逆操作红线
>
> **这块板上禁止任何一次性、不可逆的烧写** —— 不只是 eFUSE，还包括 eMMC 的
> RPMB 认证密钥、BBRAM 锁存位，以及任何 OTP 与 `*_LOCK` / `*_DISABLE` /
> `*_EN` 熔丝位。凡涉及此类动作，**必须先取得板子所有者的明确同意，默认一律
> "否"**；而且"评估过"从来不等于"可以执行"。
>
> 完整规则、熔丝清单、可逆替代，以及催生这条规则的 2026-08-18 事故：
> **[docs/SECURITY.zh-CN.md — 不可逆操作红线](docs/SECURITY.zh-CN.md#-不可逆操作红线)**

---

## 能做什么

| | |
|---|---|
| **后量子 KEM** | ML-KEM-512 / 768 / 1024（FIPS 203），KeyGen / Encaps / Decaps 全流程在 RTL 中实现——**在真硅上**与 NIST ACVP 向量逐字节一致 |
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
   │   │      │          │AES/SM4/  │ 512/768/ │ SECURE_ │ observe│  │
   │   │      │          │   SM3    │   1024   │ ONLY=1  │  only  │  │
   │   └──────┴────┬─────┴────▲─────┴──────────┴─────────┴────────┘  │
   │               └──────────┘  use_key: private wire, not the bus  │
   │      every slot sits behind axi4lite_firewall (AxPROT gate)     │
   └─────────────────────────────────────────────────────────────────┘
```

每个 slot 占 64 KB，位于 `0x8000_0000 + slot × 0x1_0000`。slot 4 是密钥仓的
第二个实例，与前一个**仅有** `SECURE_ONLY=1` 之差；它存在的全部意义就是被拒绝，
正是这一点把"门控有效"变成了证据。

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
./tools/demo_remote.sh --provision   # 只需一次：生成 PKI、装板子、留凭据
./tools/demo_remote.sh --smoke       # 之后纯本地，零 SSH
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
| AxPROT 门控，双向（测于 RAZ/WI 改动之前，当时记为 DECERR） | EL3 读 `SECURE_ONLY=1` 得到 `0x0001_0000`；EL1-NS 读同一地址被拒；同一个 EL1-NS 能读到 `SECURE_ONLY=0` 的核 |
| TRNG 最小熵，1,048,576 个调节前样本 | H = 0.871234 bit/sample → RCT 47、APT 672 |
| Decaps 时序，有效密文 vs 隐式拒绝，各 200 次 | 中位数差异 0.000 % |
| ML-KEM-512 吞吐 @ 75 MHz | 924 / 1339 / 1018 ops/s（KeyGen / Encaps / Decaps） |
| cocotb 回归 · Verilator lint · Yosys | 200 项测试 · 70 个模块，0 条告警 · 全部可综合 |

方法与原始日志见 [docs/TESTING.zh-CN.md](docs/TESTING.zh-CN.md)；板上抓取的输出
原样保存在 [board/logs/](board/logs/) 下。

## 状态与局限

关于本项目**不**做什么的准确陈述。

- **ML-KEM 私钥会离开硬件。** `KeyGen` 经 AXI 返回 `ek ‖ dk`，因为对照 ACVP 向量
  校验必须如此。守护进程留下 `dk`，交给应用的是一个句柄，所以它不会离开*接口*
  ——但"私钥永不离开硬件"目前还不成立，本项目也不这么主张。密钥仓里的对称密钥
  是另一回事：那些确实没有读出路径。
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
- ~~**ML-DSA 只有算子。**~~ **已串成整核。** ML-DSA-44/65/87 的 KeyGen/Sign/Verify
  已进位流（槽 6），并在真硅上逐字节对上 ACVP 向量。
- **没有加密启动，而且结论是确定性的。** 见
  [最终状态报告](docs/FINAL-REPORT-2026-08-17.zh-CN.md)：BBRAM 受阻于板级硬件
  （`VCC_BATT` 没接电池）；PUF 黑钥用有效测法（POR 冷启动）验过，BootROM **在碰
  PUF 之前**就拒收 —— JTAG 读 CSU 证明 `PUF_CMD=0`、`KEY_RDY=0`、OCM 从未装载。
  **RSA 认证启动倒是通的**（零 eFUSE，上板验过），但它不是信任根：`bh_auth_enable`
  的语义就是跳过 eFUSE 里的 PPK 摘要校验。**防替换的信任根只有烧 eFUSE 才有，
  而 eFUSE 是本项目的永久红线。**
- **PS 侧的 XMPU/XPPU 覆盖不到 PL 窗口。** UG1085 对此有定论：没有任何 PS 保护单元
  覆盖 `0x8000_0000`。PL 里的防火墙是这条路径上唯一的执行点——这也正是地址译码
  一一对应、不设镜像的原因。
- **DDR 侧的 XMPU 另有一条已查清的边界。** XMPU **本身不拦截事务**，它只打毒标记、
  由终点 gate（XAPP1320 v3.0 p.10）。开了 `POISON.ATTRIB` 之后，普通世界读 OP-TEE
  安全内存**确实变成 Bus error**（板上验过，功能零影响）。
  **现在它开机自动生效**：BL31 在 EL3 里把六个 XMPU_DDR 实例配好，上电 35 秒后
  `devmem 0x60000000` 就是 Bus error，**没有任何手工步骤**，在默认演示形态里。
  它挡住的**只是"普通世界读 OP-TEE 的 core 段"这一条** —— root 仍然能命令硬件做运算
  （SiP 不校验调用来源），共享内存段本来就该双向可读。详见最终状态报告。

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

英文版本以去掉 `.zh-CN` 后缀的同名文件并列存放。架构、API、寄存器、安全与测试
另有一份合订 PDF——[设计与验证参考](docs/reference/design-validation.zh-CN.pdf)，
26 页，由 `./tools/pdf/build-pdf.sh` 从同一批 Markdown 重出，因此两者不会互相
漂移。

## 参与贡献

见 [CONTRIBUTING.md](CONTRIBUTING.md)。报告漏洞前请先读
[SECURITY.md](SECURITY.md)——但请注意这是一个原型，上面列出的已知差距已经是
已知的。

## 许可证

[Apache-2.0](LICENSE)——选它而不选 MIT，是因为它有明确的专利授权与专利反制条款；
专利在这个领域是真实的风险敞口，而 MIT 对此只字未提。

`third_party/pkcs11-v3.2/` 收录了三个 OASIS 头文件，按其各自的条款原样引入。
