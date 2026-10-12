[English](README.md) · **中文**

# 综合

Out-of-context 综合脚本，产出资源占用与时序报告。

> **状态：在用。** Mac 上没有 Vivado，所以这套脚本在构建机（Vivado 2020.1）上跑。
> `docs/TESTING.zh-CN.md` 里的每一个资源占用与 Fmax 数字都出自它，且取的是**布线后**的值
> —— 综合后的 WNS 通常偏乐观。

```bash
vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs <part> <top_module>
# 例如
vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs xazu3eg-sfvc784-1-i mlkem_encaps
```

**两个 tclargs 都是必需的，part 在前、top 在后。** 漏掉 part 会直接打印用法退出，
很容易白跑一次。

可综合的顶层模块：`mont_reduce`、`barrett_reduce`、`butterfly_ct`、`butterfly_gs`
（组合逻辑，由 `ooc_comb.xdc` 约束）；`ntt_core`、`mldsa_ntt_core`、`keccak_f1600`、
`sha3_core`、`pqc_accel_axi`、`mlkem_keygen` / `mlkem_encaps` / `mlkem_decaps`、
`mlkem_cbd_stream`、`mlkem_bitpack`、`mlkem_bitunpack`、`key_vault`、`key_vault_axi`、
`axi4lite_firewall`、`aes_core`、`sm4_core`、`sm3_core`、`sym_axi`、`sym_vault_top`
（时序逻辑，由 `ooc_seq.xdc` 约束）。脚本会自动选择约束文件；清单之外的顶层名一律落到
`ooc_comb.xdc`，所以新增时序模块要先把它加进 `ooc_synth.tcl` 的清单再跑。

`keccak_f1600` 是最值得先跑的一个：它的资源占用直接检验"采用单轮迭代而非 24 轮全
展开"这一决策。1600 个状态触发器加一轮组合逻辑应当落在几千 LUT 的量级；若全展开，
逻辑要乘 24，小器件上放不下。综合报告能把这条设计判断变成一个数字。

常用器件：

| 开发板 | part |
|---|---|
| AXU3EGB（本设计实际运行的板子） | `xazu3eg-sfvc784-1-i` |
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
├── ooc_mldsa.tcl            按参数集（44/65/87）做 mldsa_<keygen|sign|verify|engine> 的 OOC 综合
├── trng_ooc.tcl             TRNG 的 OOC 综合 + 布局布线（默认顶层 trng_axi）
├── impl_bitstream.tcl       zu3eg_hsm_top 的完整非工程模式流程 -> .bit/.bin
│                            （PQC_DEV_OPEN=1 出 zu3eg_hsm_dev.bit，SECURE_ONLY=0）
├── constraints/ooc_seq.xdc  时序模块：真实 clk 端口、100 MHz、I/O delay、rst_n false path
├── constraints/ooc_comb.xdc 组合模块：虚拟时钟，仅报 in-to-out
├── constraints/trng_ro.xdc  TRNG：时钟、I/O 预算、rst_n false path
├── constraints/board_pins.xdc  AXU3EGB 管脚约束（只有风扇 PWM，AA11）
└── rpt/                     报告输出（gitignore）
```

## 约束为什么分成两份

时序模块必须通过其真实的时钟端口来约束。若使用未绑定到任何端口的虚拟时钟，`clk`
端口不受约束，任何时序路径都不会被分析，`report_timing_summary` 给出的是失真的
——而且看起来偏好的——结果。组合模块没有时钟端口，需要相反的处理方式。

`ooc_synth.tcl` 按模块性质选择对应的约束文件：

| 模块 | 约束 | 说明 |
|---|---|---|
| 上面清单里的所有时序顶层（`ntt_core`、`keccak_f1600`、`sha3_core`、`pqc_accel_axi` 等） | `ooc_seq.xdc` | `create_clock -period 10 [get_ports clk]`、I/O delay、`rst_n` false path |
| `mont_reduce`、`barrett_reduce`、`butterfly_ct`、`butterfly_gs` | `ooc_comb.xdc` | 虚拟时钟，组合路径延迟 |

**为什么是 100 MHz 而不是 150 MHz。** 蝶形中含两级串行乘法（`zeta·b`，以及
Montgomery 约减内部的 `m·Q`），再加模约减。6.667 ns 偏紧；在 100 MHz 拿到可信的正
WNS，比在 150 MHz 得到一个乐观的失败更有价值。

S3（系数搬进真双口 BRAM、蝶形尚未拆分）之后的实测：`ntt_core` 单独收敛，WNS `+0.077 ns`；
`pqc_accel_axi` 整个顶层差 `-0.047 ns`（6190 条路径里 2 条不过，Fmax 99.5 MHz）。
关键路径就是上面那条蝶形乘法链，只是操作数改由 BRAM 输出提供 ——
9.693 ns 里有 0.998 ns 是 BRAM 的 clk→out。当时考虑过两条修法：打开 BRAM 的可选输出寄存器
（`DO_REG=1`，Vivado 在综合日志里自己提示了），代价是所有读路径再加一拍延迟；
或者给蝶形打一拍，代价是每蝶形多一个周期。最后走的是第二条：`butterfly_ct` /
`butterfly_gs` 拆成 `_head` + `_tail`，`ntt_core` 在两半之间插一级寄存器
（`rtl/mlkem/ntt_core.v`），所以现在一次变换是 3457 拍。上面这些数字早于这次改动。
