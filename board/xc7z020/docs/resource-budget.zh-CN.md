[English](resource-budget.md) · **中文**

# XC7Z020 资源预算

## 结论先写

交付到 XC7Z020 的配置**不包含 ML-KEM 的 NTT 核**，只有 Keccak-f[1600] 核加总线
接口。理由是量出来的，不是估的：

| 配置 | LUT | 占 53200 的比例 | 结论 |
|---|---|---|---|
| Keccak + 总线（交付配置） | 18278 | 34.4% | 放得下，余量充足 |
| Keccak + NTT + 总线 | 58008 | 109.0% | **放不下** |

命令由 `board/xc7z020/tools/resource_budget.sh` 生成，可重跑复现。它同时对交付
配置做硬性检查：超过器件容量的 70% 即失败退出——预算是一条会失败的检查，
不是一段说明文字。

## 器件容量

XC7Z020（PYNQ-Z2、黑金 AX7020 等都是这一颗）：

| 资源 | 数量 |
|---|---|
| LUT | 53200 |
| 触发器 | 106400 |
| DSP48E1 | 220 |
| 36Kb BRAM | 140 |
| URAM | 无 |

## 实测数据

由 Yosys 综合到 7 系列单元（`synth_xilinx -family xc7 -flatten`）：

| 模块 | LUT | FF | DSP | BRAM36 | MUXF7/8 |
|---|---|---|---|---|---|
| pqc_accel_zynq（交付配置） | 18278 | 6071 | 0 | 0 | 784 |
| pqc_accel_zynq（含 NTT） | 58008 | 10271 | 13 | 0 | 23096 |
| ntt_core（ML-KEM 7 层） | 23228 | 4146 | 13 | 0 | 13182 |
| keccak_f1600 | 5864 | 1607 | 0 | 0 | 149 |
| mldsa_ntt_core（8 层） | 45659 | 8242 | 31 | 0 | 26584 |
| mlkem_rej_uniform | 6250 | 3083 | 0 | 0 | 2545 |
| mldsa_rej_uniform_buf | 2713 | 5899 | 0 | 0 | 830 |
| trng_health | 136 | 53 | 0 | 0 | 2 |
| axi4lite_regs | 392 | 243 | 0 | 0 | 7 |

AXI-DMA 与互联不在上表内，它们是 Xilinx 的 IP：`axi_dma`（simple mode、32 位）
约 1200 LUT / 1600 FF，AXI 互联每个从口约 400 LUT。

## 这些数字是什么，不是什么

**不是 Vivado 的实现结果。** Yosys 用的是开源的 7 系列单元库，打包策略与 Vivado
不同（LUT 组合进 SLICE、FF 与 LUT 共享、SRL 推断、BRAM 与分布式 RAM 的推断策略），
通常 Vivado 的 LUT 数会更低一些。上板前必须用
`board/xc7z020/vivado/build_bitstream.tcl` 生成 `utilization.rpt`，以那份为准。

**但结论不依赖两者的差距。** 交付配置只占 34%，即使 Vivado 的数字偏高一倍也
放得下；含 NTT 的配置超出 110%，即使 Vivado 能省掉三成也仍然超。两个方向上都有
足够的余量支撑结论。

## NTT 核为什么这么贵

`ntt_core` 的系数存储是 256×16，一个周期要**写两个地址**（一次蝶形产生两个输出，
分别写回 `mem[j]` 与 `mem[j+len]`）。没有任何 FPGA 的 RAM 原语支持一周期两次写：
块 RAM 与分布式 RAM 都只有一个写口。因此这块存储只能落成触发器，配上

- 3 个读口的多路选择器（`a_val`、`b_val`、`scale_in`），每个是 256 选 1、16 位宽；
- 2 个写口的地址译码器。

Yosys 报出的 13182 个 MUXF7/MUXF8 正是这些选择器树。这不是 Yosys 的推断不好，
Vivado 面对同一份 RTL 也只能这么做——**约束来自写口数量，不来自工具**。

把它放回来的前提是改写系数存储的组织方式：按 bank 拆分，让每个 bank 一个周期
只被写一次，就能映射成真正的 RAM。主干 `hardware/rtl/mlkem/ntt_core.v` 的注释里
已经写明"提高并行度需要把系数拆成多个 bank"，那一步同时会解决面积问题。
ML-DSA 的 NTT 核（45659 LUT）成本更高，同一条路径也适用。

## 裁掉 NTT 的代价

按主干 `tools/amdahl.py` 的分析，ML-KEM-768 里 SHAKE 约占 55%、NTT 相关约 30%。
单独硬件化 Keccak 的端到端加速上限约 2.09×，单独硬件化 NTT 约 1.37×。
因此在只能放一个的前提下，Keccak 本来就是第一顺位——裁掉 NTT 损失的是较小的那部分。

裁掉之后，操作码 7/8 与其它未实现的模式一样返回 `ERRCODE=3`，契约上没有新增
第二种"不支持"的表达方式。`board/xc7z020/tools/cocotb_ship_config.sh` 在
`INCLUDE_NTT=0` 下跑一遍 cocotb，确认这条行为成立、Keccak 数据面照常工作。

要重新包含 NTT：

```bash
vivado -mode batch -source board/xc7z020/vivado/create_project.tcl -tclargs -with-ntt
```

这个开关设的是板级包装 `pqc_accel_zynq` 的 `INCLUDE_NTT`，再由它透传给
`pqc_accel_axi`。**只覆盖 `pqc_accel_axi` 的参数是无效的** —— 包装层传下去的实参
会盖掉它。`resource_budget.sh` 正是因此把参数覆盖在包装层上；早先的版本覆盖在内层
模块上，结果两个配置量的是同一个设计，是脚本自带的一致性检查把它抓出来的。
在 XC7Z020 上打开这个开关会超出容量，换更大的器件（例如 XC7Z035/XC7Z045）
时才有意义。

## 时钟与时序

PL 时钟取 PS7 的 `FCLK_CLK0`，在 `create_project.tcl` 里定为 **100 MHz**。

**时序是否收敛没有任何证据。** 本仓库的开发机上没有 Vivado，从未跑过实现。
Keccak 的一轮组合逻辑（θ→ρ→π→χ→ι）是设计里最长的路径，100 MHz 下是否留得住
余量必须由实现报告回答。`build_bitstream.tcl` 会读出 WNS/WHS，为负时直接失败
退出——时序不收敛不是警告，加速器的结果会随机出错且没有规律。

若不收敛，两条路：降低 `PCW_FPGA0_PERIPHERAL_FREQMHZ`，或在 Keccak 的一轮逻辑
中间插一级流水（代价是每次置换从 24 周期变成 48 周期，对 PS 侧的调用频度仍然够）。

## 上板后要核对的三件事

1. `utilization.rpt` 与本文的估算是否在同一数量级。差得远说明推断行为与预期不同，
   要回来看是哪一块存储被映射成了别的东西。
2. `timing.rpt` 的 WNS/WHS 是否为正。
3. `.bit` 与 `.hwh` 是否同名同版本。PYNQ 靠文件名配对，错配会让 Overlay 读到旧的
   地址表，表现为寄存器读得到值但值没有意义。
