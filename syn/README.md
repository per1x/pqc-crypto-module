# syn/ —— 综合与资源/时序报告

## 状态：脚本已就位，**未经实机验证**（本机没有 Vivado）

Vivado 需要 AMD 账号且体积几十 GB，本项目的开发机上没装。
`ooc_synth.tcl` 是照 Vivado 标准 non-project 流程写的，装了之后按下面跑；
第一次可能需要按你的 Vivado 版本微调。

```bash
vivado -mode batch -source syn/ooc_synth.tcl -tclargs xck26-sfvc784-2LV-c butterfly_ct
```

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
├── ooc_synth.tcl       out-of-context 综合 + 布局布线 + 直接打印 Fmax
├── constraints/ooc.xdc 最小时钟约束
└── rpt/                报告输出（gitignore）
```
