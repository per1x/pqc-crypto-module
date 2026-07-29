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
from mldsa_model import GAMMA2_32, GAMMA2_88  # noqa: E402
from mldsa_model import Q as DQ  # noqa: E402
from mldsa_model import ZETAS as MLDSA_ZETAS  # noqa: E402
from mldsa_model import caddq as mldsa_caddq  # noqa: E402
from mldsa_model import ct_butterfly as mldsa_ct_butterfly  # noqa: E402
from mldsa_model import decompose as mldsa_decompose  # noqa: E402
from mldsa_model import gs_butterfly as mldsa_gs_butterfly  # noqa: E402
from mldsa_model import invntt_tomont as mldsa_invntt  # noqa: E402
from mldsa_model import make_hint as mldsa_make_hint  # noqa: E402
from mldsa_model import montgomery_reduce as mldsa_montgomery_reduce  # noqa: E402
from mldsa_model import ntt as mldsa_ntt  # noqa: E402
from mldsa_model import power2round as mldsa_power2round  # noqa: E402
from mldsa_model import reduce32 as mldsa_reduce32  # noqa: E402
from mldsa_model import rej_eta_coeff as mldsa_rej_eta_coeff  # noqa: E402
from mldsa_model import rej_uniform_coeff as mldsa_rej_uniform_coeff  # noqa: E402
from mldsa_model import use_hint as mldsa_use_hint  # noqa: E402
from ref_model import (  # noqa: E402
    MONT, Q, ZETAS, barrett_reduce, basemul, cbd2, cbd3, compress,
    ct_butterfly, decompress, encode12, gs_butterfly, invntt, keccak_f1600,
    montgomery_reduce, ntt, rej_pair, shake128, shake256,
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


def export_basemul(rng: random.Random) -> None:
    p = OUT / "basemul.hex"
    with p.open("w") as f:
        header(f, "NTT 域基乘：(a0+a1x)(b0+b1x) mod (x²−ζ)，Montgomery 域",
               "a0 a1 b0 b1 zeta r0 r1（均 16bit 有符号）", "同上")
        for _ in range(1000):
            a0 = rng.randrange(-Q, Q)
            a1 = rng.randrange(-Q, Q)
            b0 = rng.randrange(-Q, Q)
            b1 = rng.randrange(-Q, Q)
            z = ZETAS[rng.randrange(len(ZETAS))]
            r0, r1 = basemul(a0, a1, b0, b1, z)
            f.write(" ".join(u16(x) for x in (a0, a1, b0, b1, z, r0, r1)) + "\n")
    print(f"  {p.name}: 1000 条")


def export_compress() -> None:
    """压缩/解压：输入域只有 q 种取值，直接穷举导出而不是抽样"""
    p = OUT / "compress.hex"
    with p.open("w") as f:
        header(f, "Compress_d / Decompress_d（FIPS 203 §4.2.1）",
               "c d in out：c=0 表示压缩（in 为 [0,q) 的系数），c=1 表示解压",
               "d 为 2 位十六进制，in/out 均为 4 位十六进制无符号")
        for d in (1, 4, 5, 10, 11):
            for x in range(Q):
                f.write(f"00 {d:02x} {x:04x} {compress(x, d):04x}\n")
            for y in range(1 << d):
                f.write(f"01 {d:02x} {y:04x} {decompress(y, d):04x}\n")
    print(f"  {p.name}: {5 * Q + (2 + 16 + 32 + 1024 + 2048)} 条（穷举）")


def export_cbd(rng: random.Random) -> None:
    p = OUT / "cbd.hex"
    with p.open("w") as f:
        header(f, "中心二项分布采样（FIPS 203 Alg 8）",
               "eta rand coeffs…：eta=2 时 rand 为 8 位十六进制、8 个系数；"
               "eta=3 时 rand 为 6 位十六进制、4 个系数",
               "rand 的最低字节对应字节流里最低地址那一字节；系数为 16bit 有符号")
        for _ in range(500):
            v = rng.getrandbits(32)
            f.write(f"02 {v:08x} " + " ".join(u16(x) for x in cbd2(v)) + "\n")
        for _ in range(500):
            v = rng.getrandbits(24)
            f.write(f"03 {v:06x} " + " ".join(u16(x) for x in cbd3(v)) + "\n")
    print(f"  {p.name}: 1000 条")


def export_rej(rng: random.Random) -> None:
    p = OUT / "rej_pair.hex"
    with p.open("w") as f:
        header(f, "SampleNTT 的取候选一步（FIPS 203 Alg 7）",
               "bytes d1 d2 ok1 ok2：3 字节 → 两个 12 位候选与是否 < q",
               "bytes 为 6 位十六进制，最低字节对应最低地址；d 为 3 位十六进制")
        # 覆盖边界：让候选正好落在 q−1 / q / q+1 上
        cases = []
        for target in (0, 1, Q - 1, Q, Q + 1, 0xFFF):
            cases.append(bytes([target & 0xFF, (target >> 8) & 0x0F, 0x00]))
            cases.append(bytes([0x00, (target & 0x0F) << 4, target >> 4]))
        cases += [bytes(rng.randrange(256) for _ in range(3)) for _ in range(500)]
        for b in cases:
            d1, d2 = rej_pair(b[0], b[1], b[2])
            v = b[0] | (b[1] << 8) | (b[2] << 16)
            f.write(f"{v:06x} {d1:03x} {d2:03x} "
                    f"{1 if d1 < Q else 0} {1 if d2 < Q else 0}\n")
    print(f"  {p.name}: {len(cases)} 条")


def export_encode12(rng: random.Random) -> None:
    p = OUT / "encode12.hex"
    with p.open("w") as f:
        header(f, "ByteEncode12 / ByteDecode12：两个系数 ↔ 3 字节",
               "c0 c1 bytes：c0/c1 为 16bit 有符号，bytes 为 6 位十六进制",
               "bytes 的最低字节对应最低地址那一字节")
        for _ in range(1000):
            c0 = rng.randrange(-Q, Q)
            c1 = rng.randrange(-Q, Q)
            b0, b1, b2 = encode12(c0, c1)
            f.write(f"{u16(c0)} {u16(c1)} {b0 | (b1 << 8) | (b2 << 16):06x}\n")
    print(f"  {p.name}: 1000 条")


# ---------------------------------------------------------------- ML-DSA

def u32(x: int) -> str:
    """有符号 32 位 → 8 位十六进制（二进制补码）"""
    return f"{x & 0xFFFFFFFF:08x}"


def u64(x: int) -> str:
    return f"{x & 0xFFFFFFFFFFFFFFFF:016x}"


def export_mldsa_ops(rng: random.Random) -> None:
    p = OUT / "mldsa_ops.hex"
    with p.open("w") as f:
        header(f, "ML-DSA 的 L0 算子（q = 8380417）",
               "op a out：op=00 为 Montgomery 约减（a 为 64bit），"
               "op=01 为 reduce32，op=02 为 caddq（a 为 32bit）",
               "均为大端十六进制字面量，无字节序问题")
        for _ in range(1000):
            a = rng.randrange(-DQ * (1 << 31), DQ * (1 << 31))
            f.write(f"00 {u64(a)} {u32(mldsa_montgomery_reduce(a))}\n")
        # reduce32 的定义域是 |a| ≤ 2³¹ − 2²² − 1，向量只取定义域内的值，
        # 这样"结果与输入同余"这类性质断言才成立
        lim = (1 << 31) - (1 << 22) - 1
        for _ in range(1000):
            a = rng.randrange(-lim, lim + 1)
            f.write(f"01 {u32(a)} {u32(mldsa_reduce32(a))}\n")
        for _ in range(1000):
            a = rng.randrange(-DQ, DQ)
            f.write(f"02 {u32(a)} {u32(mldsa_caddq(a))}\n")
    print(f"  {p.name}: 3000 条")


def export_mldsa_butterfly(rng: random.Random) -> None:
    p = OUT / "mldsa_butterfly.hex"
    with p.open("w") as f:
        header(f, "ML-DSA 的 CT / GS 蝶形",
               "kind a b zeta a_out b_out：kind=00 为 CT，01 为 GS（均 32bit 有符号）",
               "同上")
        for kind, fn in (("00", mldsa_ct_butterfly), ("01", mldsa_gs_butterfly)):
            for _ in range(500):
                a = rng.randrange(-DQ, DQ)
                b = rng.randrange(-DQ, DQ)
                z = MLDSA_ZETAS[rng.randrange(1, 256)]
                ao, bo = fn(a, b, z)
                f.write(f"{kind} " + " ".join(u32(x) for x in (a, b, z, ao, bo)) + "\n")
    print(f"  {p.name}: 1000 条")


def export_mldsa_ntt(rng: random.Random) -> None:
    for name, fn in (("mldsa_ntt.hex", mldsa_ntt), ("mldsa_invntt.hex", mldsa_invntt)):
        p = OUT / name
        with p.open("w") as f:
            kind = "前向 NTT（完整 8 层）" if "inv" not in name else "逆 NTT（含 f 缩放）"
            header(f, f"ML-DSA 256 点 {kind}",
                   "一行 = 一组：256 个输入系数，空格分隔；下一行 = 256 个输出系数",
                   "系数按下标 0..255 顺序；每个 32bit 有符号，十六进制")
            for _ in range(10):
                poly = [rng.randrange(-DQ // 2, DQ // 2) for _ in range(256)]
                out = fn(list(poly))
                f.write(" ".join(u32(x) for x in poly) + "\n")
                f.write(" ".join(u32(x) for x in out) + "\n")
        print(f"  {p.name}: 10 组")


def export_mldsa_rounding(rng: random.Random) -> None:
    p = OUT / "mldsa_rounding.hex"
    with p.open("w") as f:
        header(f, "ML-DSA 的高低位拆分（Power2Round / Decompose）",
               "a p2r_a0 p2r_a1 d88_a0 d88_a1 d32_a0 d32_a1",
               "a 落在 [0, q)；a0 为 32bit 有符号，a1 为 32bit 无符号")
        cases = [0, 1, 2, 4095, 4096, 8191, 8192, DQ - 1, DQ - 2,
                 GAMMA2_88, 2 * GAMMA2_88, GAMMA2_32, 2 * GAMMA2_32,
                 43 * 2 * GAMMA2_88, 15 * 2 * GAMMA2_32]
        cases += [rng.randrange(DQ) for _ in range(2000)]
        for a in cases:
            p0, p1 = mldsa_power2round(a)
            c0, c1 = mldsa_decompose(a, GAMMA2_88)
            e0, e1 = mldsa_decompose(a, GAMMA2_32)
            f.write(" ".join(u32(x) for x in (a, p0, p1, c0, c1, e0, e1)) + "\n")
    print(f"  {p.name}: {len(cases)} 条")


def export_mldsa_hint(rng: random.Random) -> None:
    p = OUT / "mldsa_hint.hex"
    with p.open("w") as f:
        header(f, "ML-DSA 的提示位（MakeHint / UseHint）",
               "mode a e hint a1：mode=00 为 γ₂=(q−1)/88，01 为 (q−1)/32；"
               "e 为施加在低位上的扰动；a1 为 UseHint(a+e, hint) 的结果",
               "a、e 为 32bit（e 有符号）")
        n = 0
        for mode, gamma2 in (("00", GAMMA2_88), ("01", GAMMA2_32)):
            for _ in range(500):
                a = rng.randrange(DQ)
                e = rng.randrange(-gamma2, gamma2 + 1)
                a0, a1 = mldsa_decompose(a, gamma2)
                h = mldsa_make_hint(a0 + e, a1, gamma2)
                used = mldsa_use_hint((a + e) % DQ, h, gamma2)
                f.write(f"{mode} " + " ".join(u32(x) for x in (a, e))
                        + f" {h} {u32(used)}\n")
                n += 1
    print(f"  {p.name}: {n} 条")


def export_mldsa_sample(rng: random.Random) -> None:
    p = OUT / "mldsa_sample.hex"
    with p.open("w") as f:
        header(f, "ML-DSA 的两类拒绝采样",
               "kind …：kind=00 为均匀采样（bytes cand ok），"
               "kind=01 为有界采样（eta nibble coeff ok）",
               "bytes 为 6 位十六进制，最低字节对应最低地址")
        cases = []
        for target in (0, 1, DQ - 1, DQ, DQ + 1, 0x7FFFFF):
            cases.append(target & 0xFFFFFF)
        cases += [rng.getrandbits(24) for _ in range(500)]
        for v in cases:
            cand, ok = mldsa_rej_uniform_coeff(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF)
            f.write(f"00 {v:06x} {cand:06x} {ok}\n")
        for eta in (2, 4):
            for nib in range(16):
                c, ok = mldsa_rej_eta_coeff(nib, eta)
                f.write(f"01 {eta:02x} {nib:x} {u32(c)} {ok}\n")
    print(f"  {p.name}: {len(cases) + 32} 条")


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
    export_basemul(rng)
    export_compress()
    export_cbd(rng)
    export_rej(rng)
    export_encode12(rng)
    export_mldsa_ops(rng)
    export_mldsa_butterfly(rng)
    export_mldsa_ntt(rng)
    export_mldsa_rounding(rng)
    export_mldsa_hint(rng)
    export_mldsa_sample(rng)
    export_keccak(rng)
    export_shake(rng)
    cross_check()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
