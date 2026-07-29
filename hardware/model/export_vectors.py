#!/usr/bin/env python3
"""分层黄金向量导出

绝大多数人只导出整机 KAT，结果 RTL 仿真一挂就完全无法定位。
正确做法是**按硬件模块划分层次，每层都导出向量**：

  L0 算子  mont_reduce.hex   Montgomery 约减 (input, output)
  L0 算子  barrett.hex       Barrett 约减
  L1 蝶形  butterfly_ct.hex  CT 蝶形 (a, b, zeta) -> (a', b')
  L1 蝶形  butterfly_gs.hex  GS 蝶形
  L2 模块  ntt.hex           256 系数进 -> 256 系数出
  L2 模块  keccak_perm.hex   Keccak-f[1600] 1600-bit 进 -> 出
  L2 模块  shake.hex         SHAKE-128/256 (msg, outlen) -> digest

格式：每行若干个十六进制字段，`#` 开头是注释。
**Verilog 的 $readmemh 能直接读单列文件**；多列的由 cocotb 侧解析。
字节序在每个文件的注释头里写清楚 —— 特别强调这点，踩坑率极高。
"""
from __future__ import annotations

import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ref_model import (  # noqa: E402
    MONT, Q, ZETAS, barrett_reduce, ct_butterfly, gs_butterfly, invntt,
    keccak_f1600, montgomery_reduce, ntt, shake128, shake256,
)

OUT = Path(__file__).resolve().parents[2] / "vectors" / "rtl"
SEED = 20260729


def u16(x: int) -> str:
    """有符号 16 位 → 4 位十六进制（二进制补码）"""
    return f"{x & 0xFFFF:04x}"


def header(f, title: str, fields: str, endian: str) -> None:
    f.write(f"# {title}\n")
    f.write(f"# 字段：{fields}\n")
    f.write(f"# 字节序/位宽：{endian}\n")
    f.write(f"# 由 hardware/model/export_vectors.py 生成，seed={SEED}\n")


def export_mont(rng: random.Random) -> None:
    p = OUT / "mont_reduce.hex"
    with p.open("w") as f:
        header(f, "Montgomery 约减：out ≡ in · 2^-16 (mod 3329)",
               "in(32bit 有符号) out(16bit 有符号)", "均为大端十六进制字面量，无字节序问题")
        for _ in range(1000):
            a = rng.randrange(-Q * (1 << 15), Q * (1 << 15))
            f.write(f"{a & 0xFFFFFFFF:08x} {u16(montgomery_reduce(a))}\n")
    print(f"  {p.name}: 1000 条")


def export_barrett(rng: random.Random) -> None:
    p = OUT / "barrett.hex"
    with p.open("w") as f:
        header(f, "Barrett 约减：把 a 归约到 (-q/2, q/2]",
               "in(16bit 有符号) out(16bit 有符号)", "同上")
        for _ in range(1000):
            a = rng.randrange(-32768, 32768)
            f.write(f"{u16(a)} {u16(barrett_reduce(a))}\n")
    print(f"  {p.name}: 1000 条")


def export_butterfly(rng: random.Random) -> None:
    for name, fn in (("butterfly_ct.hex", ct_butterfly), ("butterfly_gs.hex", gs_butterfly)):
        p = OUT / name
        with p.open("w") as f:
            kind = "Cooley-Tukey（正向 NTT）" if "ct" in name else "Gentleman-Sande（逆 NTT）"
            header(f, f"{kind} 蝶形", "a b zeta a_out b_out（均 16bit 有符号）", "同上")
            for _ in range(1000):
                a = rng.randrange(-Q, Q)
                b = rng.randrange(-Q, Q)
                z = ZETAS[rng.randrange(len(ZETAS))]
                ao, bo = fn(a, b, z)
                f.write(f"{u16(a)} {u16(b)} {u16(z)} {u16(ao)} {u16(bo)}\n")
        print(f"  {p.name}: 1000 条")


def export_ntt(rng: random.Random) -> None:
    for name, fn in (("ntt.hex", ntt), ("invntt.hex", invntt)):
        p = OUT / name
        with p.open("w") as f:
            header(f, f"256 点 {'前向 NTT（只做 7 层）' if 'inv' not in name else '逆 NTT（含 f 缩放）'}",
                   "一行 = 一组：256 个输入系数，空格分隔；下一行 = 256 个输出系数",
                   "系数按下标 0..255 顺序；每个 16bit 有符号，十六进制")
            for _ in range(20):
                poly = [rng.randrange(-Q // 2, Q // 2) for _ in range(256)]
                out = fn(list(poly))
                f.write(" ".join(u16(x) for x in poly) + "\n")
                f.write(" ".join(u16(x) for x in out) + "\n")
        print(f"  {p.name}: 20 组")


def export_keccak(rng: random.Random) -> None:
    p = OUT / "keccak_perm.hex"
    with p.open("w") as f:
        header(f, "Keccak-f[1600] 单次置换",
               "一行 = 25 个 64bit lane（输入），下一行 = 25 个 lane（输出）",
               "lane 顺序 A[x][y] 按 index = x + 5y；每个 lane 打印为 16 位十六进制，"
               "即 lane 的数值本身（**不是**小端字节序展开）")
        cases = [[0] * 25]
        for _ in range(19):
            cases.append([rng.getrandbits(64) for _ in range(25)])
        for st in cases:
            out = keccak_f1600(list(st))
            f.write(" ".join(f"{x:016x}" for x in st) + "\n")
            f.write(" ".join(f"{x:016x}" for x in out) + "\n")
    print(f"  {p.name}: 20 组")


def export_shake(rng: random.Random) -> None:
    p = OUT / "shake.hex"
    with p.open("w") as f:
        header(f, "SHAKE-128 / SHAKE-256（含吸收与挤压的完整核）",
               "行格式：<128|256> <msg_len> <out_len> <msg_hex> <digest_hex>",
               "msg 与 digest 都是字节流的十六进制，按字节顺序")
        for bits, fn in ((128, shake128), (256, shake256)):
            for mlen in (0, 1, 31, 32, 33, 135, 136, 137, 200):
                msg = bytes(rng.randrange(256) for _ in range(mlen))
                for olen in (32, 64, 168):
                    d = fn(msg, olen)
                    f.write(f"{bits} {mlen} {olen} {msg.hex() or '-'} {d.hex()}\n")
    print(f"  {p.name}: {2 * 9 * 3} 条")


def cross_check() -> None:
    """交叉验证：本模型的 Keccak 置换必须能拼出标准库的 SHAKE 结果。

    这是 说的"两份独立实现输出一致才算可信"——
    左边是自己写的置换，右边是 hashlib。
    """
    rate = 168  # SHAKE-128
    msg = b"cross-check"
    pad = bytearray(msg) + b"\x1f"
    while len(pad) % rate:
        pad.append(0)
    pad[-1] ^= 0x80
    state = [0] * 25
    for off in range(0, len(pad), rate):
        blk = pad[off:off + rate]
        for i in range(rate // 8):
            lane = int.from_bytes(blk[i * 8:i * 8 + 8], "little")
            state[i] ^= lane
        state = keccak_f1600(state)
    out = b"".join(state[i].to_bytes(8, "little") for i in range(rate // 8))[:32]
    assert out == shake128(msg, 32), "自写 Keccak 置换与 hashlib 的 SHAKE 不一致"
    print("  交叉验证：自写 Keccak-f[1600] 拼出的 SHAKE-128 与 hashlib 逐字节一致")


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    rng = random.Random(SEED)
    print(f"导出分层黄金向量 → {OUT}")
    export_mont(rng)
    export_barrett(rng)
    export_butterfly(rng)
    export_ntt(rng)
    export_keccak(rng)
    export_shake(rng)
    cross_check()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
