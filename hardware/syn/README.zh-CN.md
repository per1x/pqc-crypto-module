[English](README.md) · **中文**

# 综合

Out-of-context 综合脚本，产出资源占用与时序报告。

> **状态：已写好，从未执行。** 开发机上没有安装 Vivado。`ooc_synth.tcl` 遵循标准的
> non-project 流程，首次运行时可能需要按你的 Vivado 版本做少量调整。

```bash
vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs <part> <top_module>
# 例如
vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs xck26-sfvc784-2LV-c butterfly_ct
```

可综合的顶层模块：`mont_reduce`、`barrett_reduce`、`butterfly_ct`、`butterfly_gs`
（组合逻辑，由 `ooc_comb.xdc` 约束）；`ntt_core` 与 `keccak_f1600`（时序逻辑，由
`ooc_seq.xdc` 约束）。脚本会自动选择约束文件。

`keccak_f1600` 是最值得先跑的一个：它的资源占用直接检验"采用单轮迭代而非 24 轮全
展开"这一决策。1600 个状态触发器加一轮组合逻辑应当落在几千 LUT 的量级；若全展开，
逻辑要乘 24，小器件上放不下。综合报告能把这条设计判断变成一个数字。

常用器件：

| 开发板 | part |
|---|---|
| Kria KV260（UltraScale+） | `xck26-sfvc784-2LV-c` |
| PYNQ-Z2 / Arty Z7-20 | `xc7z020clg400-1` |
| Arty A7-100T | `xc7a100tcsg324-1` |

## 为什么要在买板之前跑

资源占用、时序收敛与 Fmax 估算都不需要硬件，只需要指定目标器件。连
`write_bitstream` 都能跑完，只是没有地方烧录。唯一仍为估算的是功耗，它来自翻转率
模型，误差可达 2×。

其现实意义是：没有综合报告就选板子等于凭猜。如果 8 蝶形并行的 NTT 核在 XC7A100T 上
放不下，那么在下单 Arty A7 之前就应该知道。

## 不需要 Vivado 也能定下来的部分

`tools/cycle_budget.py` 从第一性原理计算 NTT 的周期预算与并行度取舍：

```
$ python3 tools/cycle_budget.py --fmax 150
ML-KEM 256 点 NTT：每层 128 个蝶形 x 7 层 = 896 次蝶形运算
  1 蝶形   ->  910 周期 -> 6.07 us
  4 蝶形   ->  238 周期 -> 1.59 us   （需要 2-4 个 BRAM bank）
  8 蝶形   ->  126 周期 -> 0.84 us   （存储端口成为瓶颈，需 4-8 个 bank）
```

预期顺序是：先用这张表定下微架构，再写 RTL，最后用 Vivado 检验计算值与综合值是否
吻合。

## 目录

```
hardware/syn/
├── ooc_synth.tcl            OOC 综合 + 布局布线，并打印 Fmax
├── constraints/ooc_seq.xdc  时序模块：真实 clk 端口、100 MHz、I/O delay、rst_n false path
├── constraints/ooc_comb.xdc 组合模块：虚拟时钟，仅报 in-to-out
└── rpt/                     报告输出（gitignore）
```

## 约束为什么分成两份

时序模块必须通过其真实的时钟端口来约束。若使用未绑定到任何端口的虚拟时钟，`clk`
端口不受约束，任何时序路径都不会被分析，`report_timing_summary` 给出的是失真的
——而且看起来偏好的——结果。组合模块没有时钟端口，需要相反的处理方式。

`ooc_synth.tcl` 按模块性质选择对应的约束文件：

| 模块 | 约束 | 说明 |
|---|---|---|
| `ntt_core`、`keccak_f1600`、`sha3_core`、`pqc_accel_axi` | `ooc_seq.xdc` | `create_clock -period 10 [get_ports clk]`、I/O delay、`rst_n` false path |
| `mont_reduce`、`butterfly_ct`、`butterfly_gs` | `ooc_comb.xdc` | 虚拟时钟，组合路径延迟 |

**为什么是 100 MHz 而不是 150 MHz。** 单周期蝶形中含两级串行乘法（`zeta·b`，以及
Montgomery 约减内部的 `m·Q`），再加模约减与一次异步 LUTRAM 读。6.667 ns 偏紧；在
100 MHz 拿到可信的正 WNS，比在 150 MHz 得到一个乐观的失败更有价值。要达到 150 MHz，
只需给蝶形打一拍——在 Montgomery 输出后插入寄存器——代价是每蝶形两个周期，这仍在
`cycle_budget.py` 的预算之内。
