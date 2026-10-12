[English](security-policy.md) · **中文**

# 安全策略（草稿）

本文是针对研究原型撰写的草稿。它采用 FIPS 140-3（ISO/IEC 19790:2012）非专有安全策略的结构，
并映射到 GM/T 0028-2014 的相应条款，目的是把现状与送检所需之间的差距显式列出来。第 13 节是差距
清单。本模块未申请也未取得任何认证，下文若干节描述的是模块当前尚未满足的要求。

本文描述当前形态：密钥由安全世界里的 OP-TEE 可信应用（TA）管理，PL 做运算，不长期保存任何密钥。

## 1. 概述

| 项目 | 取值 |
|---|---|
| 模块名称 | pqc-crypto-module |
| 模块类型 | 混合形态：密码引擎是 Xilinx XCZU3EG 的可编程逻辑（PL）；密钥管理运行在 Cortex-A53 上的 OP-TEE TA 里；主机软件是边界之外的控制面 |
| 目标安全等级 | 硬件边界已存在并生效。要主张 3 级还需要物理防拆响应、保密的密钥根和算法证书（见 §13） |
| 形态 | XCZU3EG（AXU3EGB 板）上的 FPGA 位流；Cortex-A53 上运行 Linux（普通世界）、BL31（EL3）和带 TA 的 OP-TEE（安全世界）；主机侧是共享库（`pqchsm-pkcs11`），板上有 daemon `pqchsm_fpgad`、`libsdfe`、`hsmctl`、`hsm_httpd` |
| 密码边界（本文划定的） | PL：ML-KEM 512/768/1024、ML-DSA 44/65/87、AES-128/256、SM4、SM3、环形振荡器 TRNG、对称密钥寄存器（`key_vault_axi`）、`mlkem_axi` 与 `mldsa_axi` 的单次运算展开区，以及围住它们的按 AxPROT 门控的 AXI 防火墙。密钥生成与包裹在 TA 里完成，在这条 PL 边界之外；下面的强制证据只覆盖 PL |
| 边界如何生效 | `axi4lite_firewall` 对 `SECURE_ONLY=1` 的从机拒绝任何 `AxPROT[1]=1` 的事务：读回 0、写被丢弃、不产生总线错误（RAZ/WI）。已在板上双向证明，见下方证据 |
| 已测试的运行环境 | XCZU3EG（`xazu3eg-sfvc784-1-i`）@75 MHz，Cortex-A53 上 Linux 5.4；主机侧工具在 macOS arm64 与 Debian aarch64（GCC 12） |

**边界生效的证据。** 板上实测，同一次运行、同一份位流，各取一个地址：

| | `0x8004_0000`（`SECURE_ONLY=1`） | `0x8003_0000`（`SECURE_ONLY=0`） |
|---|---|---|
| 安全世界 EL3（`AxPROT[1]=0`） | 读到 `0x00010000` | 读到 `0x00010000` |
| 普通世界 EL1-NS（`AxPROT[1]=1`） | 被拒：读回 `0`，写被丢弃，不产生总线错误 | 读到 `0x00010000` |

右边一列是对照：同一个普通世界能成功读到没有门控的核，所以左边那格的拒绝来自门控，而不是地址不可达。

这张表来自更早的一份位流，当时槽 3（`mlkem_axi`）是 `SECURE_ONLY=0`。现在的默认位流里所有功能从机都是
`SECURE_ONLY=1`（由 `zu3eg_hsm_top.v` 的 `SECURE_ONLY_FUNCTIONAL` 一处决定），右列的对照点已经不存在。
要重做这个对照，用开发位流（`PQC_DEV_OPEN=1`，功能从机 `SECURE_ONLY=0`）作右列，槽 4 的金丝雀作左列。
左列在默认位流上于 2026-08-17 复验过：普通世界读槽 0、1、2、3、6 全部回 0
（`board/logs/RESULT_secform_mldsa.txt`）。安全世界那一侧是一个最小的 EL3 负载：给 BL31 加的一个 SiP 调用，
只做一次读并把值返回。原始日志与方法见 [SECURITY.zh-CN.md](../SECURITY.zh-CN.md)。

另有一项针对对称密钥寄存器的实测：两个实例各扫 256 字节（48 个读得到、80 个被防火墙拒），密钥的四个字
一个都没出现，而这些密钥算出的密文是正确的。第三项测量是在开发位流上由普通世界直接写装载口：11 次写
全部被丢弃，`LOAD_REJ` 从 0 变成 11，槽位仍为空。

模块提供后量子密钥封装与数字签名、对称与国密算法、以 blob 形式保存的密钥、槽位与会话管理、备份与恢复，
以及防篡改审计日志，对外有 PKCS#11 v3.2 接口（主机栈）、SDF 风格接口、命令行和 HTTP 网关（板上）。

## 2. 密码模块规格

主机栈由 `src/` 与 `include/pqchsm/` 下的源码定义，构建为 `libpqchsm` 与 PKCS#11 共享库。它链接
OpenSSL 3。后量子算法来自自带的 mlkem-native / mldsa-native（`third_party/pqc-native/`），OP-TEE TA
也用它们。这些库和 OpenSSL 在 PL 边界之外，其算法实现原样使用。

各算法核及其参数集见 [ARCHITECTURE.zh-CN.md](../ARCHITECTURE.zh-CN.md#密码核)，验证状态见
[TESTING.zh-CN.md](../TESTING.zh-CN.md)。

模块只有一种工作模式，不存在非核准模式：需要非核准算法的服务一概不提供。

加速器抽象层（`include/pqchsm/accel.h`）允许把算术运算交给可编程逻辑执行。四种 transport 以相同方式实现它：
软件桩、仿真 RTL 后端、经 AXI4-Lite 与 AXI4-Stream 驱动同一批 RTL 的后端，以及面向真实设备的内存映射
transport。它们共用的寄存器契约见 [REGISTERS.zh-CN.md](../REGISTERS.zh-CN.md)。

在已测试的运行环境上，主机栈（`src/`）走的是软件桩。服务层那条路（`service/`、daemon、`/dev/secmmio`、EL3、
PL）是真硬件，ML-KEM、ML-DSA、AES、SM4、SM3 和 TRNG 都在板上对过官方向量（见
[TESTING.zh-CN.md](../TESTING.zh-CN.md) 与 `board/logs/`）。两句话同时成立，覆盖表见
[API.zh-CN.md](../API.zh-CN.md)。

## 3. 密码模块接口

| 逻辑接口 | 实现方式 |
|---|---|
| 数据输入 | PKCS#11 函数参数；TLV 命令协议（`src/proto`）；SDF 风格调用（`libsdfe`）；`hsmctl` 的文件；HTTP 请求 |
| 数据输出 | PKCS#11 返回缓冲；TLV 响应；SDF 返回值；HTTP 响应 |
| 控制输入 | PKCS#11 函数选择；命令行与管理工具命令；daemon 的线协议操作 |
| 状态输出 | PKCS#11 返回码；`hsm_status_t`；审计日志；板上的 `HSM_STATUS` |
| 电源接口 | 软件部分不适用 |

`include/pqchsm/` 中没有函数、应用接口（`libsdfe`、`hsmctl`、HTTP）也没有一个返回私钥材料。
`include/pqchsm/` 中的每个 API 都基于不透明句柄工作；`tools/check_no_readback.py` 在结构上强制这一点：
一旦有人往密钥派生根的头文件里加入读回函数，检查即失败。唯一的例外在 PL 寄存器层：由软件提供种子的
KeyGen（ACVP 路径）会经 `OUT_DATA` 返回私钥，见 §13。

## 4. 角色、服务与鉴别

主机栈定义三种角色（`hsm_role_t`）：

| 角色 | 鉴别方式 | 服务 |
|---|---|---|
| 公共 | 无 | 读取公开的令牌信息、读取公钥、验签 |
| 用户 | 用户 PIN | 密钥生成、签名、封装、解封装、槽位内的对象管理 |
| 安全管理员 | 管理员 PIN | 令牌初始化、PIN 重置与解锁、清零、备份与恢复 |

鉴别基于 PIN。PIN 以带槽位盐值的 KMAC256 摘要存储，比较使用常量时间原语。用户 PIN 连续失败会累加计数并
在达到上限时锁定槽位，只有安全管理员可解锁。

角色由每个服务入口都要过的显式访问控制表强制，槽位状态机（`src/slot/fsm.c`）是一张显式转移表。两者都由
`tests/unit/test_slot_fsm.c` 直接覆盖。

板上的服务路径（`pqchsm_fpgad`、`hsmctl`、`hsm_httpd`）没有角色和 PIN。本机 UNIX socket（权限 0600）和
回环 HTTP 端口（无鉴别）信任板上能连到它们的进程；远程口和 `:8443` 要求客户端证书。能以 `LOGIN_USER`
身份打开 TA 会话的进程，可以让 TA 装载它手里 blob 对应的密钥。按调用者授权没有实现。

## 5. 软件安全

模块以源码交付，由集成方构建。构建产物的完整性由运行环境负责，模块自身不对自己做签名校验。送检需要对模块
映像采用核准的完整性技术，本模块尚无此机制。板上的启动链（BOOT.BIN、BL31、OP-TEE、TA）没有经过认证：没有
烧 eFUSE，而不烧 eFUSE 的 RSA 认证启动不构成信任根。

主机栈的 keystore 文件带整文件 KMAC256 标签，因此记录的删除、调换与截断都会在装载时被拦住。审计日志是
SHA3-256 哈希链，链头由 ML-DSA 设备身份密钥签名。

## 6. 运行环境

这一节说的是主机软件栈，不是 PL 边界。两者是不同的东西：

* 密码边界是 PL 里的核加防火墙，是固定的硬件配置，换掉它要重出位流。
* 主机软件栈（`src/`、PKCS#11 模块）在边界之外，是可修改型，运行在通用操作系统上，按 FIPS 140-3 对应
  安全 1 级，进程隔离由操作系统负责。
* daemon（`service/`）也在边界之外，是普通世界驱动 PL 的唯一一方。TA 运行在 OP-TEE 的安全世界里，它的
  完整性取决于启动链。

所以主张几级要看指的是哪一部分。边界本身有硬件门控的证据（§1），但整机 3 级还缺物理防拆响应和保密的
密钥根（§13）。

敏感缓冲在平台允许时 `mlock`，使用后清零。两者都不能防御能读取进程地址空间的攻击者；审计覆盖的范围见
[constant-time.zh-CN.md](constant-time.zh-CN.md)。

## 7. 物理安全

不存在物理安全。PL 有一根 `tamper` 输入，拉高会清空对称密钥寄存器和其他核。它由 SYSMONE4 的过温输出驱动，
需要该信号连续高 2^16 个周期才生效，这条路径没有在板上触发过。没有防拆外壳、防拆响应和环境失效保护。
Zynq 级 SoC 上的独立形态需要它们。

## 8. 非入侵式安全

不作声明。常量时间审计（`tools/ct_audit.py`）验证 `src/` 下模块自身源码中不存在秘密相关的分支、内存下标与
除数，`tests/unit/test_ct_timing.c` 以统计方式验证常量时间比较原语与数据无关。两者都不构成 ISO/IEC 19790
附录 F 意义上的非入侵式攻击缓解声明：OpenSSL、自带的 mlkem-native / mldsa-native 和 RTL 都在审计之外，
也没有做过任何功耗或电磁分析。

## 9. 敏感安全参数管理

| 参数 | 生成 | 存放 | 清零 |
|---|---|---|---|
| 对称密钥（AES、SM4） | 在 TA 里（OP-TEE PRNG 与 PL TRNG 字混合） | TA 之外只以 `PWRP` blob 存在（KEK 下的 AES-256-GCM） | 在 PL 密钥寄存器槽里保持到会话结束，然后由 `ZEROIZE` 清除（一个时钟）；`tamper` 时同样清除 |
| 后量子私钥种子（ML-KEM `d‖z`、ML-DSA `ξ`） | 在 TA 里 | 以 `PWRP` blob 与公钥一起存放。PL 不存种子 | 暂存的种子用一次后由硬件清零；展开区在每次运算结束时由硬件擦除（`KEYSTAT[0]`） |
| KEK | 在 TA 里派生：HUK，再到 TA 唯一密钥，再用每设备 salt 做 `KMAC256` | 不存储，缓存在 TA 内存里 | TA 实例销毁时 |
| HUK | `SHA-256(Device DNA)`，不烧 eFUSE | 不存储 | 不适用。Device DNA 可读 |
| 主机栈：KDR、KEK、RMK、BEK、PIN 密钥 | 见 [ARCHITECTURE.zh-CN.md](../ARCHITECTURE.zh-CN.md#密钥层次) | keystore 文件（已包裹），RMK 只以 Shamir 分片存在 | `tools/check_zeroize.py` 检查每个字段 |

密钥层次见 [ARCHITECTURE.zh-CN.md](../ARCHITECTURE.zh-CN.md#密钥层次)。

**熵源。** 边界内有一个带 SP 800-90B 连续健康检测的环形振荡器噪声源（`hardware/rtl/trng/`）。TA 取密钥熵时，
把 OP-TEE PRNG 与 TRNG 字混合（SiP `0x8200ff17`，健康检测报警时返回错误）。主机栈里，`pqc_random_bytes()`
硬件优先，取不到就报错，不静默回退（`src/util/util.c`），那里的七种密钥材料（私钥种子、PIN 密钥与盐、备份
RMK、Shamir 系数、KEK 盐、包裹 nonce、KEM-DEM 的 IV）全部走它。只有在没有安装 transport 的宿主上（纯软件
构建、单元测试）才回落到由操作系统播种的 OpenSSL DRBG，调用方可以通过 `hwrng_available()` 看到这一点。原始
噪声的最小熵实测 0.871234 比特/样本，重启矩阵见 `board/logs/RESULT_restart.txt`。

主机内存的清零是结构性的：`tools/check_zeroize.py` 验证每个密钥材料字段都被其销毁函数抹掉、每个局部密钥缓冲
在每条返回路径上都被抹掉，否则构建失败。`tests/unit/test_zeroize.c` 另在 `-O2` 下验证清零原语没有被当作死
存储消除，并配有对照，确认同一位置上的普通 `memset` 确实会被消除。TA 里解开的密钥用完后用 `pqchsm_bzero`
清除。

PL 内部的清零分两种：

- 对称密钥寄存器是寄存器阵列：`zeroize` / `tamper` 在一个时钟沿清完全部槽位，不存在擦了一半的窗口。选寄存器
  而不是 BRAM，这是理由之一。会话结束时 daemon 会发出 `ZEROIZE`。
- ML-KEM 的缓冲以及 `mlkem_axi`、`mldsa_axi` 的单次运算展开区是 BRAM，只能逐地址写 0（8192 拍），擦了一半的
  窗口一定存在。设计上把这个窗口暴露成状态位（`WIPING`，展开区用 `KEYSTAT[0]`）。置位期间拒绝读写、拒绝
  启动，写回 SLVERR 而不是静默丢弃，因为静默丢弃会让软件按错误的长度启动，得到一个安静的错误结果。仿真里的
  判据是读回两块 BRAM 的全部 16384 个字节确认为 0，不是 `OUT_LEN` 变成 0（后者只说明软件读不到）。

  这一条在板上证不了，因为软件本来就没有任何路径能读到那些字节。板上能证的是 `WIPING` 的行为与时长。

PS 侧的 XPPU 与 FPD_XMPU 覆盖不到 `0x8000_0000` 那段 PL 窗口（UG1085 v2.5 表 16-10 列出了全部 aperture；
`M_AXI_HPM0_LPD` 按 p1092 绕过 FPD）。所以 PL 里按 AxPROT 门控的防火墙是这条路由上唯一的强制点，也正因如此，
总线译码必须做到地址一一对应、不留镜像。

## 10. 自测试

主机栈的上电自测在第一次密码服务之前运行，也可显式重跑（`pqc_self_test()`）。每一项都是已知答案测试，
期望值来自模块之外：

| 测试项 | 期望值来源 |
|---|---|
| SHA3-256 | FIPS 202 公开示例 |
| KMAC256 | SP 800-185 官方示例 Sample #4 |
| AES-256-GCM | GCM 规范测试用例 14 |
| ML-KEM-768 密钥生成 | NIST ACVP ML-KEM-keyGen-FIPS203 向量 |
| ML-DSA-65 密钥生成 | NIST ACVP ML-DSA-keyGen-FIPS204 向量 |

任一项失败即置模块为错误状态，此后每个密码服务都返回 `PQC_ERR_SELF_TEST` 且不产生任何输出，恢复需要重跑并
通过。`tests/unit/test_selftest.c` 验证错误状态确实拦下每一个入口，包括参数本身非法的情况。

板上的 daemon 启动时不运行这些。板上的 24 项自检（`hsm_hwtest`，结果见 `board/logs/RESULT_hwtest.txt`）和
ACVP 运行都是手动启动的。

FIPS 140-3 要求但尚未实现的条件自测试：新生成密钥对的配对一致性测试，以及随机数发生器的连续测试。两者都
列在 §13。

## 11. 生命周期保障

开发全程版本控制，每次改动都带上对应的测试。测试套件在 macOS/arm64 与 Debian/aarch64 上通过 `ctest` 运行，
并在 AddressSanitizer、UndefinedBehaviorSanitizer 下运行，并发路径另在 ThreadSanitizer 下运行。算法路径的
正确性钉在 NIST ACVP 向量上，`tools/kat_evidence.sh` 通过实际运行重新生成证据表。

RTL 由 272 个用例的 cocotb 回归对着独立预言机验证，并由 `tools/rtl_lint.sh` 检查（Verilator `-Wall`，每个
模块各自作为顶层跑一遍，外加一道 Icarus Verilog）。`rtl_lint.sh` 目前不通过：七个模块（`mldsa_axi`、
`mldsa_engine`、`mldsa_keygen`、`mldsa_sign`、`mldsa_verify`、`mlkem_axi`、`zu3eg_hsm_top`）仍有 Verilator
警告。豁免条目连同理由列在 `hardware/rtl/lint_waivers.vlt` 中，按文件与信号名精确匹配，因此同类告警的新增
实例不会被顺带遮住。

交付形态为源码。没有签名的发布产物，没有正式的配置管理记录，除仓库文档之外也没有单独的操作员指南。

## 12. 对其他攻击的缓解

除 §8 与 [constant-time.zh-CN.md](constant-time.zh-CN.md) 所述的常量时间性质外，不作其他声明。

## 13. 与送检要求之间的差距

下列每一条都足以单独阻断送检。列在这里是为了避免把本文读成「已经就绪」。

1. **密钥根与芯片绑定，但不保密。** 板上的 HUK 是 `SHA-256(Device DNA)`，没有烧 eFUSE。Device DNA 可以通过
   JTAG 读到，拿到 DNA 和 blob 的人可以解开 blob。主机栈里，DEV 构建的 KDR 是编译进去的常量（PRODUCTION
   构建在没有提供根密钥时会失败）；`PQCHSM_KDR=device-dna` 把它绑定到 DNA，能防克隆，但不提供保密。保密的
   根需要基于 eFUSE 的认证启动。
2. **边界内的熵源：已关闭。** 边界内有一个带 SP 800-90B 连续健康检测的环形振荡器噪声源，实测最小熵
   0.871234 比特/样本。没有安装 transport 时，主机栈仍从 OpenSSL 取种子。
3. **没有模块完整性校验。** 使用前没有任何机制校验模块映像。
4. **缺少条件自测试。** 没有新生成密钥对的配对一致性测试，也没有随机数连续测试。
5. **没有物理安全。** 边界靠逻辑（AxPROT 门控）生效。没有防拆外壳、防拆响应和环境失效防护。`tamper` 输入
   只接了 SYSMONE4 的过温输出（见 §7），这条路径没有被触发过。
6. **没有算法证书。** ACVP 向量是本地跑的，这是正确性证据，不是证书。
7. **审计日志假定单写者。** 审计模块不对文件加锁。
8. **Shamir 分片校验和不带密钥。** 只能发现损坏，不能发现篡改。
9. **管理员 PIN 未强制锁定。** 失败会计数但不锁定槽位，因为锁定等于把设备变砖，兜底手段是 M-of-N 恢复。
10. **ML-DSA 的完整硬件实现：已关闭。** ML-DSA 44/65/87 的 KeyGen/Sign/Verify 有完整整核，挂在 `mldsa_axi`
    （槽 6）后面，在板上对 NIST ACVP 向量逐字节一致（32/32，见 `board/logs/` 与
    [TESTING.zh-CN.md](../TESTING.zh-CN.md)）。ML-KEM 512/768/1024 与对称核（AES-128/256、SM4、SM3）同样在
    板上对上了各自的标准向量。
11. **ACVP KeyGen 路径会输出私钥。** 由软件提供 `d‖z` 或 `ξ` 的 KeyGen 经 `OUT_DATA` 返回私钥。
    `CTRL.SEED_LOCK` 可以把 KeyGen 限制为只走种子口，但它是只有断电才能清除的闩锁，daemon 不设置它。
12. **root 仍然可以让硬件做运算。** daemon 经 EL3 白名单发出运算序列，SiP 不检查调用者。密钥装载只在安全世界
    进行，所以普通世界既读不到也注入不了密钥，但可以使用已经装载的密钥。DDR 侧的 XMPU 只能阻止读（它给事务
    打毒标记，由终点拒绝）。
13. **没有认证启动或加密启动。** BBRAM 被板子卡死（`VCC_BATT` 没有电池），PUF 黑钥被 BootROM 拒收。要防止镜像
    被替换需要烧 eFUSE，而本项目不做不可逆烧写。见 [SECURITY.zh-CN.md](../SECURITY.zh-CN.md)。

## 与 GM/T 0028-2014 的对应

GM/T 0028-2014 与 ISO/IEC 19790:2012 对齐，因此章节结构高度对应。下表记录每条要求在本文中的落点。

| GM/T 0028 条款 | 主题 | 本文对应 |
|---|---|---|
| 7.1 | 密码模块规格 | §1、§2 |
| 7.2 | 密码模块接口 | §3 |
| 7.3 | 角色、服务和鉴别 | §4 |
| 7.4 | 软件/固件安全 | §5 |
| 7.5 | 运行环境 | §6 |
| 7.6 | 物理安全 | §7 |
| 7.7 | 非入侵式安全 | §8 |
| 7.8 | 敏感安全参数管理 | §9 |
| 7.9 | 自测试 | §10 |
| 7.10 | 生命周期保障 | §11 |
| 7.11 | 对其他攻击的缓解 | §12 |

有两处差异影响 GM/T 0028 送检，本模块都没有处理。该标准的核准算法清单是商用密码算法体系（SM2、SM3、SM4、
ZUC）。SM4 与 SM3 在 PL 里，已在板上对过官方向量（GB/T 32907 A.1 与 GB/T 32905 A.1）。SM2 与 ZUC 既无核也无
软件实现，SM2 是面向 GM/T 送检的头号缺口，缺口清单见 [API.zh-CN.md](../API.zh-CN.md)。第二处差异是随机性要求
引用的是 GM/T 0005 而非 SP 800-90B，因此 `hardware/rtl/trng/` 里的噪声源健康检测阈值需要按该文件重新推导。
