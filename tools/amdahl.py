#!/usr/bin/env python3
"""把 profiling 的占比变成硬件切分决策（路线图 §5.2.3）

§5.2 的目的不是"知道 SHAKE 慢"，而是算出**各种硬件切分方案的端到端加速比上界**，
据此决定先做哪个核。这个脚本就是那张表的生成器。

用法：
    tools/amdahl.py                       # 用路线图 §5.2.3 的文献占比
    tools/amdahl.py --keccak 0.48 --ntt 0.34   # 用你自己测出来的占比

⚠️ 默认占比来自路线图正文（ML-KEM-768：SHAKE ~55%、NTT 相关 ~30%），
**不是本项目的实测值** —— 本机上的符号级归因失败了，原因见 doc/profiling_report.md。
拿到目标平台的实测数字后，用 --keccak/--ntt 覆盖再跑一遍。
"""
from __future__ import annotations

import argparse


def speedup(keccak: float, ntt: float, kec_hw: float, ntt_hw: float,
            transfer: float = 0.0) -> float:
    """Amdahl：原时间归一化为 1，加速后的时间是各部分之和。

    kec_hw / ntt_hw 是该部分的硬件加速倍数（1 = 不加速）。
    transfer 是 PS-PL 搬运引入的**额外**开销（占原总时间的比例）。
    """
    rest = 1.0 - keccak - ntt
    t = keccak / kec_hw + ntt / ntt_hw + rest + transfer
    return 1.0 / t


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--keccak", type=float, default=0.55,
                    help="Keccak/SHAKE 占比（默认 0.55，路线图文献值）")
    ap.add_argument("--ntt", type=float, default=0.30,
                    help="NTT 相关占比（默认 0.30，路线图文献值）")
    ap.add_argument("--kec-speedup", type=float, default=20.0, help="Keccak 核加速倍数")
    ap.add_argument("--ntt-speedup", type=float, default=10.0, help="NTT 核加速倍数")
    ap.add_argument("--measured", action="store_true",
                    help="声明占比是实测值（仅影响报告里的措辞）")
    a = ap.parse_args()

    k, n = a.keccak, a.ntt
    if k + n > 1.0:
        print("占比之和超过 1")
        return 2
    rest = 1.0 - k - n
    src = "实测" if a.measured else "文献值（非本项目实测）"

    print(f"占比来源：{src}")
    print(f"  Keccak/SHAKE {k:.0%}   NTT 相关 {n:.0%}   其余 {rest:.0%}")
    print(f"  假定加速倍数：Keccak {a.kec_speedup:.0f}×，NTT {a.ntt_speedup:.0f}×")
    print()

    rows = [
        ("① 全软件（基线）",            1.0,            1.0,            0.00),
        ("② 只做 NTT 硬件",             1.0,            a.ntt_speedup,  0.00),
        ("③ 只做 Keccak 硬件",          a.kec_speedup,  1.0,            0.00),
        ("④ 两个核都做",                a.kec_speedup,  a.ntt_speedup,  0.00),
        ("⑤ 两个都做 + 10% 传输开销",   a.kec_speedup,  a.ntt_speedup,  0.10),
        ("⑥ 两个都做 + 20% 传输开销",   a.kec_speedup,  a.ntt_speedup,  0.20),
        ("⑦ 整条流程进 PL（无搬运）",   1e9,            1e9,            0.00),
    ]
    print(f"{'方案':<28}{'剩余时间':>10}{'端到端加速比':>14}")
    print("-" * 54)
    for name, kh, nh, tr in rows:
        s = speedup(k, n, kh, nh, tr)
        print(f"{name:<28}{1.0/s:>10.3f}{s:>13.2f}×")

    print()
    only_ntt = speedup(k, n, 1.0, a.ntt_speedup)
    only_kec = speedup(k, n, a.kec_speedup, 1.0)
    both = speedup(k, n, a.kec_speedup, a.ntt_speedup)
    both_tr = speedup(k, n, a.kec_speedup, a.ntt_speedup, 0.20)

    print("结论：")
    first = "Keccak" if only_kec > only_ntt else "NTT"
    print(f"  1. 单独做 NTT 只有 {only_ntt:.2f}×，单独做 Keccak 有 {only_kec:.2f}× "
          f"→ **先做 {first}**（若目标是端到端性能）。")
    print(f"     若目标是学 RTL/格密码结构，先做 NTT 仍然合理 —— 这是两个不同的目标。")
    print(f"  2. 两个都做能到 {both:.2f}×；但 20% 的 PS-PL 搬运开销把它压到 {both_tr:.2f}×，"
          f"吃掉 {100*(both-both_tr)/both:.0f}%。")
    print(f"     ⇒ 接口设计（DMA、批处理粒度、是否整条 KeyGen 都进 PL）必须在**架构阶段**决策，")
    print(f"       不能等 RTL 写完再补救。")
    print(f"  3. 上界：即使把整条流程搬进 PL，也不会超过 {speedup(k, n, 1e9, 1e9):.0f}× 之外的"
          f"任何幻想 —— Amdahl 的天花板由'其余 {rest:.0%}'决定。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
