[English](README.md) · **中文**

# 综合

用于模块级 out-of-context（OOC）综合和整板 bitstream 的 Vivado 脚本。Mac 上没有
Vivado，脚本在构建机（Vivado 2020.1）上运行。Fmax 由布线后的 WNS 推算，因为综合后的 WNS
通常偏乐观。报告写到 `hardware/syn/rpt/`（已 gitignore），bitstream 产物写到
`hardware/syn/impl/`。

## 文件

| 文件 | 用途 |
|---|---|
| `ooc_synth.tcl` | 对单个模块做 OOC 综合与布局布线，打印 Fmax 并写 `rpt/<top>_fmax.txt` |
| `ooc_mldsa.tcl` | 同样的流程，对象是 `mldsa_keygen`、`mldsa_sign`、`mldsa_verify` 或 `mldsa_engine`；报告按参数集命名 |
| `trng_ooc.tcl` | TRNG 的 OOC 综合与布局布线（默认顶层 `trng_axi`） |
| `impl_bitstream.tcl` | `zu3eg_hsm_top` 的非工程模式流程，在 `syn/impl/` 产出 `.bit` 和 `.bin` |
| `constraints/ooc_seq.xdc` | 时序模块：`clk` 端口 100 MHz，输入输出延迟 1 ns，`rst_n` 为 false path |
| `constraints/ooc_comb.xdc` | 组合模块：虚拟时钟，只约束输入到输出的路径 |
| `constraints/trng_ro.xdc` | TRNG：时钟、I/O 预算、`rst_n` 与 `tamper` 的 false path |
| `constraints/board_pins.xdc` | AXU3EGB 管脚约束，只有风扇 PWM 的 `AA11` 一根 |

## ooc_synth.tcl

```bash
vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs <part> <top_module>
# 例：
vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs xazu3eg-sfvc784-1-i mlkem_encaps
```

两个参数都必须给，part 在前。缺任何一个，脚本打印用法后退出。

脚本读入 `rtl/common`、`mlkem`、`mldsa`、`keccak`、`bus`、`sym` 和 `trng`。下表中的顶层
使用 `ooc_seq.xdc`，其他名字一律使用 `ooc_comb.xdc`，所以新增的时序顶层要先加进
`ooc_synth.tcl` 里的列表。

| 约束文件 | 顶层 |
|---|---|
| `ooc_seq.xdc` | `ntt_core`、`mldsa_ntt_core`、`keccak_f1600`、`sha3_core`、`pqc_accel_axi`、`mlkem_keygen`、`mlkem_encaps`、`mlkem_decaps`、`mlkem_cbd_stream`、`mlkem_bitpack`、`mlkem_bitunpack`、`key_vault`、`key_vault_axi`、`axi4lite_firewall`、`aes_core`、`sm4_core`、`sm3_core`、`sym_axi`、`sym_vault_top` |
| `ooc_comb.xdc` | 其余所有，用于 `mont_reduce`、`barrett_reduce`、`butterfly_ct`、`butterfly_gs` |

时序模块必须通过它真实的时钟端口来约束。如果用一个没有绑定端口的虚拟时钟，`clk` 就没有
约束，没有路径被计时，时序摘要会比实际情况好看。

OOC 时钟用 100 MHz（10 ns）而不是 150 MHz，因为 ML-KEM 蝶形里有两级串行乘法（`zeta·b`
和 Montgomery 约减里的 `m·Q`）再加一次模约减。板上密码核运行在 75 MHz。`ntt_core` 在
`_head` 与 `_tail` 之间插了一级寄存器，所以一次变换是 3457 拍。

## ooc_mldsa.tcl

```bash
vivado -mode batch -source hardware/syn/ooc_mldsa.tcl \
       -tclargs <part> <keygen|sign|verify|engine> <44|65|87>
# 例：
vivado -mode batch -source hardware/syn/ooc_mldsa.tcl -tclargs xazu3eg-sfvc784-1-i sign 87
```

ML-DSA 的核在运行时选参数集，所以一次综合同时覆盖 44、65、87。最后一个参数只用来给报告
命名（`mldsa_<op>_<pset>_{utilization,timing}_{synth,route}.rpt`）。

## trng_ooc.tcl

```bash
vivado -mode batch -source hardware/syn/trng_ooc.tcl
vivado -mode batch -source hardware/syn/trng_ooc.tcl -tclargs <part> <top>
```

脚本读入 `trng`、`common` 和 `keccak`。环振网络允许组合环，DRC `LUTLP-1` 降为警告。综合后
和布线后各数一次 `u_ro*` 下的 LUT 数，少于 120 就报错（8 条环、每条 13 到 27 级，共 160
个）。环被优化掉时 TRNG 会变成常数发生器，而日志里没有任何错误。不要定义
`TRNG_SIM_MODEL`，它会把 `ring_osc` 换成无法综合的仿真模型。

## impl_bitstream.tcl

```bash
vivado -mode batch -source hardware/syn/impl_bitstream.tcl                 # zu3eg_hsm.bit
PQC_DEV_OPEN=1    vivado -mode batch -source hardware/syn/impl_bitstream.tcl   # zu3eg_hsm_dev.bit
PQC_CHARACTERIZE=1 vivado -mode batch -source hardware/syn/impl_bitstream.tcl  # zu3eg_hsm_char.bit
```

流程先生成 PS（`zynq_ultra_ps_e`，使用 `M_AXI_HPM0_LPD` 和 150 MHz 的 `pl_clk0`），读入
`rtl/*` 与 `platform/fan_ctrl`，然后依次做综合、`opt_design`、`place_design`、
`phys_opt_design`、`route_design`、保持时间修复和 `write_bitstream`。这里的 PS 配置只决定
PL 侧的端口；板上实际的 PS 设置来自 `BOOT.BIN` 里厂家的 `psu_init`。

| 变体 | 环境变量 | 产物 |
|---|---|---|
| 默认 | 无 | `zu3eg_hsm.bit`：功能从机 `SECURE_ONLY=1` |
| 开发 | `PQC_DEV_OPEN=1` | `zu3eg_hsm_dev.bit`：功能从机 `SECURE_ONLY=0`；密钥寄存器装载口和种子口仍只接受安全写 |
| 表征 | `PQC_CHARACTERIZE=1` | `zu3eg_hsm_char.bit`：风扇阈值压低、TRNG 原始抽头打开；不是产品形态 |

下列任一检查失败，脚本会在 `write_bitstream` 之前停下：

- 网表里找不到 `tamper_latched` 触发器（`tamper` 通路被优化成了常量）。
- `fan` 端口不在 `AA11`，或者 `SYSMONE4` 缺失、`SIM_DEVICE` 不是 `ZYNQ_ULTRASCALE`。
- PS 的时钟或应答输入（`maxihpm0_lpd_aclk`、`maxigp2_rlast`、`maxigp2_bid`、`maxigp2_rid`）
  无驱动。Vivado 对先用后声明的 wire 不报错，只会建一条无驱动的网络。
- 建立时间 WNS 或保持时间 WHS 为负，或有效保持余量低于 0.050 ns。脚本在系统时钟上加了
  0.100 ns 的保持不确定度，让布线器给过短的路径插延迟；报告里的 WHS 已经扣除了它。

时序结果写到 `syn/impl/timing_<name>.txt`。`.bin` 是 `fpga_manager` 和 `fpgautil` 加载用的
原始比特流。

## 默认 bitstream 的结果

`zu3eg_hsm.bit` 在 `xazu3eg-sfvc784-1-i` 上、75 MHz 的布线后结果（构建机报告，2026-10-10）：

| 资源 | 用量 |
|---|---|
| LUT | 55,366（78.5 %） |
| 寄存器 | 39,990 |
| Block RAM tile | 85 |
| DSP | 199 |
| 建立时间 WNS | +1.615 ns |

## 器件

| 板子 | 器件 |
|---|---|
| AXU3EGB（本项目使用的板子） | `xazu3eg-sfvc784-1-i` |
| Kria KV260 | `xck26-sfvc784-2LV-c` |
| PYNQ-Z2 / Arty Z7-20 | `xc7z020clg400-1` |
| Arty A7-100T | `xc7a100tcsg324-1` |

`xazu3eg-sfvc784-1-i` 是 AXU3EGB 上的车规 XA 器件，资源与 XCZU3EG 相同：70,560 LUT、
141,120 FF、360 DSP、216 个 BRAM36。

## 写 RTL 之前的估算

`tools/cycle_budget.py` 计算 1 到 16 路并行蝶形下 ML-KEM NTT 的周期预算，并给出粗略的 DSP
和 bank 数估计：

```
$ python3 tools/cycle_budget.py --fmax 150
```

它只覆盖 NTT。`tools/amdahl.py` 根据 profiling 占比估算不同硬件切分方案（Keccak、NTT）的
端到端加速比。
