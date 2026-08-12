# ZU3EG PL 密码硬件 — 进展

> 分支 `zu3eg-fpga-crypto`，独立 worktree。
> 与 OP-TEE 隔离主线（分支 `board/zu3eg`）**共用同一个仓库和同一块开发板**，
> 但**不共用 checkout**：那条线的进展记在桌面 `axu3egb-secure-report/进展报告.md`，
> 本文件只记 PL 侧，两边不互相写。

## 目标与边界

按真商用密码机的架构，把能塞进 ZU3EG PL 的密码硬件做出来并接进系统。
**目的是原型验证，不追性能** —— 门槛是"能实现、能塞进这颗片子"。

**目标器件：`xazu3eg-sfvc784-1-i`**（车规 XA 版，不是 XC）。
这是从构建机上出厂 Vivado 工程 `~/factory_vivado/board_test.xpr` 里挖出来的
真实器件号。资源与 XCZU3EG 相同：70560 LUT / 141120 FF / 360 DSP /
216 个 BRAM36。7020 时代的资源假设一律不适用。

**工具链分工**：RTL 与 cocotb 仿真在 Mac（iverilog + verilator + cocotb 2.0.1
都在本地，仓库原有对拍流程也在这儿）；Vivado 2020.1 综合/布局布线在构建机
`ssh -p 2222 root@192.168.50.191`。**两者都不需要板子、不需要 JTAG。**
构建机是 Ubuntu 18.04 + Python 3.6，装不动现代 cocotb，所以仿真不放那边。

**上板一律先停下问用户**，与 OP-TEE 会话串行安排板子时间。

## 落地顺序

| 步 | 内容 | 状态 |
|---|---|---|
| S1 | 硬件 TRNG：环振熵源 + SP 800-90B 健康检测 + Keccak 调理 + AXI | ✅ 完成（未上板） |
| S1b | 软件侧：熵源驱动 + 替掉软件 RNG | ✅ 完成（未上板） |
| S2 | SHA-3 / SHAKE 海绵包装，把 padding/absorb/squeeze 从 C 挪进 PL | ✅ 核完成（未接总线） |
| S3 | NTT 核改 BRAM 存储版（现有版本是寄存器阵列，7020 时代的取舍） | 待做 |
| S4 | 完整 ML-KEM 核纯 RTL（keygen/encaps/decaps 状态机） | 待做 |
| S5 | 密码边界：PL BRAM 密钥保险库 + AXI firewall + zeroize-on-tamper 汇总 | 待做 |

---

# S1 硬件 TRNG（2026-08-12，完成，未上板）

## 做了什么

仓库里原本只有 `trng_health.v`（SP 800-90B §4.4 的 RCT + APT，阈值参数化、
告警锁存，质量很好）—— 但**缺噪声源本身**，也缺调理和总线接口。补齐成完整熵源：

```
环振阵列 ──采样/抽取──> 原始比特 ──┬──> 连续健康检测（RCT + APT）
                                    └──> Keccak 海绵调理 ──> FIFO ──> AXI ──> 安全世界
```

| 文件 | 作用 |
|---|---|
| `hardware/rtl/trng/ring_osc.v` | 单条环振。综合走真反相器环，仿真走抖动行为模型 |
| `hardware/rtl/trng/trng_source.v` | 8 条环振（13/15/…/27 级）+ 两级同步器 + 抽取 |
| `hardware/rtl/trng/trng_cond.v` | Keccak 海绵调理器，复用已有的 `keccak_f1600` |
| `hardware/rtl/trng/trng_top.v` | 启动健康检测、告警处置、zeroize 策略 |
| `hardware/rtl/trng/trng_axi.v` | AXI4-Lite 从机 + **AxPROT 安全门控** + tamper 引脚 |
| `hardware/rtl/common/sync_fifo.v` | 可擦除 FIFO（flush 逐地址覆零，不是只挪指针） |
| `hardware/model/trng_cond_model.py` | 调理器的 Python 黄金模型 |
| `docs/trng-register-map.zh-CN.md` | 寄存器表与驱动契约 |

## 几个有意的设计选择

**调理用 Keccak 海绵，不是自己发明的白化。**
SP 800-90B §3.1.5.1.2 给了一份 vetted conditioning 清单（HMAC/CMAC/CBC-MAC/
Hash_df/哈希函数），用清单里的构件，输出熵可以直接按 `min(输出长度, 输入熵)` 计；
自己发明的白化（von Neumann、LFSR 打散）要另行论证。
选 Keccak 还顺带证明了 `keccak_f1600` 是可复用的 IP，而不是只能给
`pqc_accel_axi` 用的一次性件 —— **零增量面积拿到一个 vetted 构件**。

**健康检测吃的是抽取之后的样本流**，也就是调理器实际消费的那一条。
检测的对象必须和被使用的对象是同一个，否则检测没有意义。

**告警之后连熵池一起清。**
标准只要求"停止输出并上报"，动作留给使用方。这里选最保守的：
告警 → ready 拉低 → FIFO 擦除 → **调理器连同海绵状态一起复位** → 启动检测重跑。
理由是告警意味着噪声源可能已经失效了一段时间，池子里可能混进了低熵输入，
留着比丢掉风险大。代价只是重新暖机。

**AxPROT 门控：被拒的读绝不弹 FIFO。**
`AxPROT[1]=1`（non-secure）的读写一律 DECERR，且不产生任何副作用。
返回 DECERR 而不是 OKAY+0 是有意的 —— 静默返回 0 会让普通世界以为拿到了
随机数，而 0 是最糟的"随机数"。
"被拒的读不弹 FIFO"单独测一条：如果被拒的读仍然弹出，普通世界虽然拿不到数，
却获得了一个"反复读把熵池抽干"的手段。让安全世界拿不到随机数，
和自己拿到随机数，是两个不同的攻击，后者一样致命。

**这一层不是唯一一层。** AxPROT 是纵深防御的最内层，完整隔离还要靠
XMPU/XPPU（在 master 那端就挡掉）和地址映射（不出现在普通世界设备树里）。
三层的分工写在 `docs/trng-register-map.zh-CN.md`。

## 综合结果（构建机 Vivado 2020.1，`xazu3eg-sfvc784-1-i`，OOC，含布局布线）

| 项 | 值 | 占比 |
|---|---|---|
| CLB LUT | **4238** | 6.01 %（共 70560） |
| CLB 寄存器 | **2540** | 1.80 %（共 141120） |
| Block RAM | **0** | 0 %（共 216） |
| DSP | 0 | 0 % |
| WNS @ 100 MHz | **+5.669 ns** | 时序全过 |
| 估算 Fmax | **230.9 MHz** | 目标 100 MHz，余量充足 |
| 片上功耗（估） | 0.298 W（动态 0.077 W） | 估算模型，误差可达 2× |

`All user specified timing constraints are met.`

LUT 主要花在 Keccak 上（单轮迭代的 f[1600] 组合逻辑），环振只占 160 个。
**整个 TRNG 只用掉这颗片子 6% 的 LUT**，后面 S2–S4 的空间很宽裕。

复现：
```bash
ssh -p 2222 root@192.168.50.191
sudo -H -u build bash -lc "cd ~/fpga_trng && \
  source /tools/Xilinx/Vivado/2020.1/settings64.sh && \
  vivado -mode batch -source hardware/syn/trng_ooc.tcl"
```

## 环振被优化掉是最危险的失效模式，脚本里专门防了

反相器环违反了综合工具的两条基本假设（组合逻辑无环、逻辑可化简）。
不做三件事，环就会被优化掉：

1. RTL 里 `DONT_TOUCH`（在 `ring_osc.v` 的 `chain` 声明上）；
2. `ALLOW_COMBINATORIAL_LOOPS TRUE`（在 `trng_ooc.tcl` 里对环网络设置）；
3. `LUTLP-1` DRC 降级为 Warning，否则 `write_bitstream` 直接失败。

**环被吃掉之后，综合日志里不会有任何报错** —— 采样器采到常量，TRNG 静默变成
常数发生器。所以 `trng_ooc.tcl` 在**综合后和布线后各数一次** ring_osc 层次下的
LUT 数量，少于 120 就直接 `error` 退出。实测两次都是 **160 个 LUT**
（8 条环 × 平均 20 级 = 160），与设计值精确吻合。

> 顺带一个坑：XDC **不是完整的 Tcl**。`if`、`for`、`remove_from_collection`
> 都会报 `[Designutils 20-1307]` 并**静默忽略那一条约束**，脚本还继续跑完。
> 第一版把条件判断写进 XDC，三条约束全被吞了。带判断的部分必须放在 `.tcl` 里。

## 验证：21 个 cocotb 用例，全通

在 Mac 上跑（`hardware/tb/cocotb/Makefile.trng`，与主 Makefile 分开 ——
环振仿真要 ps 级时间单位）：

| 文件 | 用例 | 测什么 |
|---|---|---|
| `test_trng_cond.py` | 4 | **与 Python Keccak 海绵模型逐字对拍**，含全零输入、状态推进、背压 |
| `test_trng_source.py` | 3 | 抽取比、enable 门控、采样通路没接坏 |
| `test_trng_top.py` | 4 | 启动检测前零输出、第一个字不早于 1088 比特、zeroize |
| `test_trng_axi.py` | 6 | 寄存器契约、UNDERRUN 锁存、**AxPROT 门控**、tamper |
| `test_trng_top_alarm.py` | 4 | 告警处置（用 `-P` 把 RCT 阈值压到 2 让告警确定性发生） |

```bash
cd hardware/tb/cocotb
make -f Makefile.trng MODULE=test_trng_cond      TOPLEVEL=trng_cond
make -f Makefile.trng MODULE=test_trng_source    TOPLEVEL=trng_source
make -f Makefile.trng MODULE=test_trng_top       TOPLEVEL=trng_top
make -f Makefile.trng MODULE=test_trng_axi       TOPLEVEL=trng_axi
make -f Makefile.trng MODULE=test_trng_top_alarm TOPLEVEL=trng_top \
     PARAMS=-Ptrng_top.RCT_CUTOFF=2
```

对拍里最有分量的一条是调理器：同一条比特流喂给 RTL 和 `trng_cond_model.py`，
挤出的每个 32 位字都相等。海绵是有状态的，一个字对不上后面全错，
所以这一条实际上把比特序、lane 顺序、异或注入、置换时机全钉住了。

---

## ⚠️ 尚未成立的事（不要在任何报告里写成结论）

**1. 最小熵完全没有实测。**
RTL 仿真里环振跑的是 `ring_osc.v` 的行为模型，**抖动量是编的**，比真实器件
大一到两个数量级 —— 这样做只是为了让下游数字逻辑能在合理仿真时长里被跑到。
因此：

> **任何最小熵数字都不能从仿真里得出。**
> 测试里"看起来很随机"的断言（1 的比例 0.507、最长游程 10）
> **只是在防低级错误**（采样器接反、异或写成或、抽取计数器差一），
> 不构成任何熵评估。

当前假设 **H = 0.5 bit/样本**，健康检测阈值（RCT C=41、APT W=1024 C=793）和
调理比例（1088 比特进 → 256 比特出）都是照它算的。上板之后必须：
1. 导出 ≥1M 个原始比特（`DECIM` 可调）；
2. 跑 NIST EntropyAssessment（SP 800-90B 官方工具）；
3. 按实测的 H 反过来定 `DECIM`、`RCT_CUTOFF`、`APT_CUTOFF`、`ABSORB_BLOCKS`。

**2. 环振的物理布局没有约束。**
真实设计要用 `LOC`/`pblock` 把各条环分散摆放，避免互锁（injection locking）
和电源耦合。当前是 OOC 综合，布局由工具自选。集成进完整工程时要补。

**3. 没上过板。** 以上全部是仿真 + 综合结果。

**4. 软件通路只到"驱动写完并通过契约测试"，没有跑在真硬件上。**
见下面 S1b。真板上还要做地址分配、决定 SECURE_ONLY 的取值、并把选择
transport 这一步接进 pqchsmd / TA 的初始化。

---

# S1b 软件侧：熵源驱动，替掉软件 RNG（2026-08-12，完成，未上板）

RTL 做完只是一半。用户要的是"作为熵源，让安全世界软件从它取随机、替掉软件
RNG" —— 这一半在软件树里。

## 做法：照抄 accel 那条接缝

仓库里已经有一套成熟的做法（`include/pqchsm/accel.h`）：**先把 AXI 寄存器表
定死，再写一个 C 实现的"假外设"**，暴露与真 PL 完全相同的寄存器语义，
真板到手后换成 `/dev/mem` + mmap，上层一行不改。熵源照搬这条接缝：

| 文件 | 作用 |
|---|---|
| `include/pqchsm/hwrng.h` | 寄存器偏移、状态位、错误码、`hwrng_transport_t` |
| `src/hal/hwrng.c` | 驱动本体：自检、取字节、告警处置 |
| `src/hal/hwrng_stub.c` | `trng_axi` 寄存器语义的软件模型（FIFO 由 OpenSSL 填） |
| `src/hal/hwrng_mmap.c` | `/dev/mem` + mmap，基址由 `PQCHSM_HWRNG_MMAP_BASE` 给出 |
| `tests/unit/test_hwrng.c` | 契约测试，41 条断言 |

**熵源单开一条接缝，没有并进 `accel.h` 的操作码**，与 RTL 侧分成两个 AXI 从机
是同一组理由：生命周期不同（加速器是发命令等完成，TRNG 常开自由运行）、
访问权限不同、故障域不同（TRNG 告警要能独立上报，不该被加速器的忙闲挡住）。

## 驱动里三条不能省的纪律

1. **读 RDATA 之前必须先查 `STATUS.DATA_VALID`。** 空读返回 0，而 0 是最糟的
   "随机数"。契约里那个锁存的 `UNDERRUN` 位就是用来事后抓住不查就读的驱动的。
2. **取完一批之后复查 `UNDERRUN` 与 `ALARM`，任一置位则整批清零作废。**
   不是"把出错之后的部分丢掉" —— 告警是**锁存的电平**，只说明"这段时间里发生
   过"，定位不到是第几个字。既然定位不了，就只能全丢。
3. **绝不回退到软件随机源。** `pqc_random_bytes()` 装了 transport 就走硬件，
   取不到就返回 -1；liboqs 的随机源回调签名是 `void`，没有返回值可用，
   所以那边直接 `abort()`。静默回退会让"熵来自硬件"恰好在最要紧的时刻悄悄
   变成假话，而调用方无从知道。

第 2 条是最容易写漏的一条，所以单独做了**变异测试**验证它真的在起作用：
把复查那几行换成 `(void)s;` 重新编译，`test_hwrng` 立刻在"取到最后才告警"
那条上失败（返回 0 而不是 -3，且缓冲区里留着数据）。测试确实咬得住这行代码，
不是摆设。

## 验证

`tests/unit/test_hwrng.c`，41 条断言，`ctest -R hwrng` 通过。验的是
`docs/trng-register-map.zh-CN.md` 那份契约的**软件侧** —— RTL 侧由
`test_trng_axi.py` 验同一份契约。两边对着同一张表各写各的，接口层面的分歧
在无板阶段就会暴露，这正是先定寄存器表再写假外设的收益。

覆盖到的：没装 transport 时如实报不可用、启动检测没过时 READY 不拉高、
非 4 倍数长度的尾部处理、告警早退与告警复查两条路径都整批清零、空池超时
（而不是死等、也不是硬读 RDATA 把 0 当随机数）、ZEROIZE 重跑启动检测、
`pqc_random_bytes()` 确实弹了硬件的字（读 `WORDS` 计数验证：32 字节正好 8 个
字）、故障时确实不回退。

> **这里一个字都没测熵。** transport 是软件模型，FIFO 由 OpenSSL 填 ——
> 测出来的"随机性"只反映 OpenSSL。软件模型也如实把 `is_hardware` 报成 0，
> 审计里"熵来自硬件"这句话的依据就是这一位。

模型还有一处刻意的不保真：软件里没有时钟，所以拿"读了几次 STATUS"当时间
基准（每次推进 64 个启动样本，暖机需轮询 16 次）。它证明了驱动确实在轮询、
确实等到 READY 才读；**没有**证明任何与时延、吞吐、熵率有关的东西。

## 还差什么（都要等板子）

- 地址分配：`PQCHSM_HWRNG_MMAP_BASE` 现在没有取值，`hwrng_transport_mmap()`
  返回 NULL。
- `hwrng_mmap.c` 走 `/dev/mem`，也就是跑在普通世界。而 `trng_axi` 默认
  `SECURE_ONLY=1`，**这条路在默认配置下本来就打不通**（AxPROT[1]=1 → DECERR）。
  这不是 bug：它的正当用途是综合成 `SECURE_ONLY=0` 的调试位流做通路验证和
  熵采集，以及当作 OP-TEE TA 侧驱动的原型（TA 里换成安全世界的映射，寄存器
  时序完全一样）。生产配置下取熵必须走安全世界。
- 把"选哪个 transport"接进 pqchsmd / TA 的初始化 —— 那要等 OP-TEE 主线的 TA
  稳定后再合过来。

---

# S2 SHA-3 / SHAKE 海绵进 PL（2026-08-12，核完成，未接总线）

## 做了什么

仓库里原本只有 `keccak_f1600` 置换核，海绵的 framing 全在 C 里
（`src/hal/pqc_accel.c` 的 `accel_shake`）。那个分工在当时是对的 —— 核更小，
SHAKE128/256 与 SHA3-256/512 共用同一个置换。代价是**每置换一次就要过一次
总线**：PS 写 200 字节状态 → 触发 → 读回 200 字节。一次 SHAKE256 吸收 32 字节
要搬 400 字节总线数据，去做 24 个周期的运算。

`hardware/rtl/keccak/sha3_core.v` 把 padding / 吸收 / 挤压整个搬进 PL。
除了省掉那些搬运，还有一条更要紧的：**中间状态不再离开密码边界**。
SHAKE 的中间状态在 ML-KEM 里就是 ρ/σ 展开的上下文，让它反复出入普通世界
可寻址的缓冲区，在真密码机里是说不过去的。

接口是**两端都带背压的字节流**：`in_valid/in_ready/in_data` 喂消息，
`in_flush` 宣告结束，`out_valid/out_ready/out_data` 按需挤压。
挤压**不设长度寄存器** —— SHAKE 的输出长度本来就是任意的，设了等于凭空
加一个上限，还多一个要校验的参数。消费方读够了停下即可。

## 两个刻意的简化

**吸收按字节做，不攒 lane。** `keccak_f1600` 的读口是组合的、写口在空闲时
接受写入，于是"读出 lane → 异或进一个字节 → 写回"整个是一条组合路径，
一个周期就能做完。攒 lane 的写法要处理"不足 8 字节的尾巴怎么对齐"，
按字节做连这个问题都不存在，pad 的位置计算也跟着消失。

**padding 只要两次读-改-写。** pad10*1 中间那串 0 异或进去是空操作，可以整个
跳过，只剩：① 当前位置异或 suffix；② `rate-1` 处异或 `0x80`。两笔顺序执行，
所以 suffix 恰好落在 `rate-1` 时（消息长 ≡ rate-1）自然合并成 `suffix^0x80`，
不需要单独判这个边角。

## 综合结果（`xazu3eg-sfvc784-1-i`，OOC，含布局布线）

| 项 | 值 | 占比 |
|---|---|---|
| CLB LUT | **3824** | 5.42 % |
| CLB 寄存器 | **1653** | 1.17 % |
| Block RAM / DSP | 0 / 0 | 0 % |
| WNS @ 100 MHz | **+4.813 ns** | 时序全过 |
| 估算 Fmax | **192.8 MHz** | |
| 片上功耗（估） | 0.224 W（动态 0.004 W） | |

`All user specified timing constraints are met.`

海绵包装几乎是白送的：面积主要还是 `keccak_f1600` 那份单轮组合逻辑，
外面这层状态机只多了千把个 LUT。**S1 + S2 合计约占这颗片子 11% 的 LUT**，
BRAM 一片没用 —— S3/S4 的空间还很宽裕。

## 验证：7 个用例，黄金模型直接用 hashlib

```bash
cd hardware/tb/cocotb && make -s MODULE=test_sha3_core TOPLEVEL=sha3_core
```

黄金模型**不是自己写的海绵**，是 Python 的 `hashlib.sha3_256/shake_128/…`。
自己写的模型和 RTL 可能一起错 —— 同一个人对同一处规范的同一个误读会在两边
同时出现；`hashlib` 是独立实现、被无数人验证过的。这是"黄金模型必须独立于
被测物"的实际落点。

覆盖 4 个参数集 × 边界长度（`rate-2 / rate-1 / rate / rate+1 / 2·rate`）、
空消息、跨块长挤压、背压、连算两次、置换途中 zeroize、12 组随机向量。
padding 的 bug 几乎全藏在那几个边界长度里，中间长度反而最不容易出错。

顺带把 TRNG 那 21 个用例也接进了 `tools/rtl_sim.sh`（此前它们只能手动跑，
ctest 根本没覆盖）。现在 `./tools/rtl_sim.sh` 一把跑完 **106 个测试，41 秒**。

## 这一步踩的两个坑，都值得记下来

**坑一：`start` 只在空闲时才认，于是第二次操作永远发不动。**
挤压是没有自然终点的（SHAKE 输出长度任意，核心无从知道消费方何时读够），
所以挤完也不会自己回空闲。第一版把 `start` 写在 `S_IDLE` 分支里，结果第一次
操作之后核心永远停在 `S_SQUEEZE`。表现是**驱动写了 start 之后 `in_ready`
永远不来** —— 不报错，只挂死。现在 `start` 的语义改成"丢掉手头的一切，
从干净海绵重新开始"，从任何状态都生效。

**坑二：`kec_done` 是电平，会保持到下一次 `start`，于是判完成判到了上一次。**
进入等待状态的头一拍，`start` 才刚发出去、置换还没开始，而 `done` 上挂着的
还是**上一次**置换留下的 1，于是当场判定"已完成"，在置换进行当中就开始挤压。

这个 bug 的表现极具迷惑性：A 阵列在 24 轮里每拍都在变，挤出来的**头十几个
字节取自中间轮的状态，后面的字节等置换跑完才取、反而是对的**。于是摘要
"前面错、后面对"，看着像位序或对齐问题，其实是时序问题。而且**第一次操作
恰好是对的**（复位后 `done=0`，没有陈旧值），非要连算两次才暴露 ——
所以"连算两次"那条用例不是锦上添花，它是唯一能抓住这个 bug 的用例。

> 有意思的是 `trng_cond.v` 里早就用 `!kick_busy && kec_done` 处理过同一个坑，
> 注释也写了。同一个人隔几天在同一个置换核上又踩了一遍 —— 说明这类"电平型
> done"的接口本身就容易误用。真要根治应该让 `keccak_f1600` 直接输出 `busy`。

**还有一条是测试台自己的教训**：第一版等 `in_ready` 用的是裸 `while True`，
核心一停住，仿真就闷头跑了一千六百万个周期直到被外面杀掉，什么信息都没留下。
换成带上限的等待之后，同一个 bug 变成一条指出 `state` 的失败信息，
整套用例也从"十分钟跑不完"变成 **2.6 秒**。

## 还差什么

- **没接总线。** 现在 `sha3_core` 是个裸核，PS 够不到它。要么给它单独一个
  AXI 从机（像 `trng_axi` 那样），要么在 `pqc_accel_axi` 里加一个 SHAKE 操作码
  让 `accel_shake()` 整条委托给 PL。后者能直接兑现"软件 PQC 调用硬件 SHAKE"，
  且复用现成的数据缓冲通路，但要动已经测过的总线模块。
- 没上过板。
