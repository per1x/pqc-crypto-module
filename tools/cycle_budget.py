#!/usr/bin/env python3
"""NTT cycle 预算表

特别指出：这张表**在没有任何硬件、甚至没有 RTL 的情况下就能算出来**。
它决定微架构（几个蝶形单元、几个 BRAM bank），而微架构必须在写 RTL 之前定下来。

算例：ML-KEM 的 256 点 NTT 做 **7 层**（只到 2 次多项式，不是完整 8 层），
每层 128 个蝶形 → 共 896 个蝶形运算。

用法：tools/cycle_budget.py [--fmax 150] [--alg mlkem|mldsa]
"""
from __future__ import annotations

import argparse

# (名字, 点数, 层数, 模数, 说明)
ALGS = {
    "mlkem": ("ML-KEM", 256, 7, 3329,
              "只做 7 层：NTT 到 2 次多项式为止（FIPS 203），不是完整 8 层"),
    "mldsa": ("ML-DSA", 256, 8, 8380417,
              "做完整 8 层；模数 23 bit，乘法器比 ML-KEM 宽一档"),
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fmax", type=float, default=150.0, help="目标频率 MHz（默认 150）")
    ap.add_argument("--alg", choices=sorted(ALGS), default="mlkem")
    ap.add_argument("--pipeline-fill", type=int, default=14,
                    help="流水线填充+排空的额外周期（默认 14）")
    a = ap.parse_args()

    name, points, layers, q, note = ALGS[a.alg]
    per_layer = points // 2
    total_bf = per_layer * layers

    print(f"{name} 的 {points} 点 NTT cycle 预算  @ {a.fmax:.0f} MHz")
    print(f"  {note}")
    print(f"  每层 {per_layer} 个蝶形 × {layers} 层 = **{total_bf} 个蝶形运算**")
    print(f"  模数 q = {q}（{q.bit_length()} bit）")
    print()
    print(f"{'蝶形并行度':>10}{'cycles':>10}{'延迟(µs)':>12}{'NTT/秒':>12}   瓶颈提示")
    print("-" * 72)
    for par in (1, 2, 4, 8, 16):
        cycles = total_bf // par + a.pipeline_fill
        us = cycles / (a.fmax * 1e6) * 1e6
        rate = 1e6 / us
        if par <= 2:
            hint = "乘法器空闲少，面积最小"
        elif par == 4:
            hint = "需 2–4 个 BRAM bank"
        elif par == 8:
            hint = "**存储端口成为瓶颈，需 4–8 bank**"
        else:
            hint = "bank 冲突与布线开始主导，收益递减"
        print(f"{par:>10}{cycles:>10}{us:>12.2f}{rate:>12.0f}   {hint}")

    print()
    dsp = {1: 1, 2: 2, 4: 4, 8: 8, 16: 16}
    print("粗略资源含义（每个蝶形 1 次模乘 → 约 1 个 DSP48，ML-DSA 因位宽可能 2 个）：")
    mult = 2 if a.alg == "mldsa" else 1
    for par in (1, 4, 8):
        print(f"  {par:>2} 蝶形并行 ≈ {dsp[par]*mult:>2} DSP48 + {max(2, par//2)} 个系数 bank")
    print()
    print("怎么用这张表：")
    print("  1. 先定端到端目标（比如 KeyGen < 1 ms），反推 NTT 需要多快；")
    print("  2. 从表里挑满足要求的最小并行度 —— 并行度越高，存储端口越难做；")
    print("  3. 把选中的并行度记进设计说明，再开始写 RTL。")
    print("  ⚠️ 这里只算了 NTT。若 Keccak 才是占比更大的那一段，")
    print("     那么先做 Keccak 核才是端到端收益更高的选择（见 tools/amdahl.py）。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
