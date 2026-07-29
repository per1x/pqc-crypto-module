# syn/ —— 综合与资源/时序报告

## 状态：脚本已就位，**未经实机验证**（本机没有 Vivado）

Vivado 需要 AMD 账号且体积几十 GB，本项目的开发机上没装。
`ooc_synth.tcl` 是照 Vivado 标准 non-project 流程写的，装了之后按下面跑；
第一次可能需要按你的 Vivado 版本微调。

```bash
vivado -mode batch -source syn/ooc_synth.tcl -tclargs xck26-sfvc784-2LV-c butterfly_ct
```

可综合的顶层：`mont_reduce`、`barrett_reduce`、`butterfly_ct`、`butterfly_gs`
（纯组合，走 `ooc_comb.xdc`）；`ntt_core`、**`keccak_f1600`**（有 clk 端口，
走 `ooc_seq.xdc`）。

`keccak_f1600` 是最值得先跑的一个：**它的资源占用直接检验"单轮迭代而不是
24 轮展开"这个决策**。1600 个状态触发器 + 一轮组合逻辑应当在小几千 LUT 的
量级；如果 24 轮展开，逻辑要乘 24，小器件上放不下 —— 这正是当初选单轮的理由，
拿到综合报告就能把它从"设计判断"变成"有数字支撑的结论"。

常用 part：

| 板子 | part |
|------|------|
| Kria KV260（推荐，US+） | `xck26-sfvc784-2LV-c` |
| PYNQ-Z2 / Arty Z7-20 | `xc7z020clg400-1` |
| Arty A7-100T | `xc7a100tcsg324-1` |

## 为什么要在买板子之前跑

路线图 §5.0 的表明确写着：**资源占用、时序收敛与 Fmax 估算都不需要板子**，
只需指定 part；连 `write_bitstream` 都能跑，只是没地方烧。
唯一只能估的是功耗（基于翻转率的模型值，误差可达 2×）。

反过来说 —— 也是 §5.0 的原话 —— **在没有综合报告的情况下就买板子，
板子只会吃灰**。这一步的产出直接决定选哪块板：如果 8 蝶形并行的 NTT 核
在 XC7A100T 上放不下，那就别买 Arty A7。

## 不需要 Vivado 也能先算的部分

`tools/cycle_budget.py` 给出 NTT 的 cycle 预算与并行度取舍
（§5.8.1 说这张表"在没有任何硬件、甚至没有 RTL 的情况下就能算出来"）：

```
$ python3 tools/cycle_budget.py --fmax 150
ML-KEM 的 256 点 NTT：每层 128 个蝶形 × 7 层 = 896 个蝶形运算
  1 蝶形并行 →  910 cycles → 6.07 µs
  4 蝶形并行 →  238 cycles → 1.59 µs   （需 2–4 个 BRAM bank）
  8 蝶形并行 →  126 cycles → 0.84 µs   （存储端口成为瓶颈，需 4–8 bank）
```

先用它定微架构（写进 `doc/uarch.md`），再写 RTL，最后才用 Vivado 验证
"算出来的和综合出来的对不对得上"。

## 目录

```
syn/
├── ooc_synth.tcl            OOC 综合 + 布局布线 + 直接打印 Fmax
├── constraints/ooc_seq.xdc  时序模块（ntt_core）：绑 clk 端口，100 MHz + I/O delay
├── constraints/ooc_comb.xdc 纯组合模块：虚拟时钟，只报 in2out
└── rpt/                     报告输出（gitignore）
```

## 约束为什么分两份（这是一处真会误导人的坑）

早先只有一份 `ooc.xdc`，里面是个**没有绑定任何端口的虚拟时钟**，
注释还写着"当前 rtl/mlkem 下都是纯组合逻辑"。那在 `ntt_core` 写出来之后就不成立了：
用它综合 `ntt_core`，`clk` 端口上没有时钟约束 → 所有时序路径不被计时 →
`report_timing_summary` 给出的是**失真/空**的结果，反而让人误判"时序很好"。

现在按模块性质分流（`ooc_synth.tcl` 会自己选）：

| 模块 | 约束 | 说明 |
|------|------|------|
| `ntt_core` | `ooc_seq.xdc` | `create_clock -period 10 [get_ports clk]` + I/O delay + `rst_n` false path |
| `mont_reduce` / `butterfly_ct` / `butterfly_gs` | `ooc_comb.xdc` | 虚拟时钟，报组合路径延迟 |

**为什么是 100 MHz 而不是 150 MHz**：单周期蝶形里有**两级串行乘法**
（`zeta·b` 与 `mont` 内的 `m·Q`）加模约减加 LUTRAM 异步读。
150 MHz（6.667 ns）偏紧，先在 100 MHz 拿到可信的正 WNS 更有意义；
若要冲 150 MHz，把蝶形打一拍（`mont` 输出后插流水寄存器）即可，
代价是每蝶形 2 拍、cycle 数翻倍但仍在 `cycle_budget.py` 的预算内。
