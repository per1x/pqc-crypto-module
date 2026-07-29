#!/usr/bin/env python3
"""ML-KEM 的独立 Python 参考模型

【为什么要独立重写一遍】
只导出整机 KAT，RTL 一挂就完全无法定位。要求**按硬件模块分层**导出向量，
而且要用一份**独立实现**去证伪自己对 C 实现的理解偏差 ——
两份独立实现输出逐字节一致，才认定为可信黄金模型。

本文件只覆盖 ML-KEM 侧硬件最关心的几层：
  L0 算子：Montgomery / Barrett 约减
  L1 蝶形：CT / GS 蝶形
  L2 模块：256 点 NTT / INTT
  L2 模块：Keccak-f[1600] 单次置换（用 hashlib 交叉验证 SHAKE）

参数取 FIPS 203：q = 3329，ML-KEM 的 NTT 只做 **7 层**（到 2 次多项式为止，
不是完整 8 层）—— 这是最容易写错的一点。
"""
from __future__ import annotations

import hashlib

Q = 3329
QINV = 62209          # q^-1 mod 2^16
MONT = 2285           # 2^16 mod q
F = 1441              # mont^2 / 128

# FIPS 203 的 zeta 表（Montgomery 域），与参考实现一致
ZETAS = [
    -1044, -758, -359, -1517, 1493, 1422, 287, 202, -171, 622, 1577, 182, 962,
    -1202, -1474, 1468, 573, -1325, 264, 383, -829, 1458, -1602, -130, -681,
    1017, 732, 608, -1542, 411, -205, -1571, 1223, 652, -552, 1015, -1293, 1491,
    -282, -1544, 516, -8, -320, -666, -1618, -1162, 126, 1469, -853, -90, -271,
    830, 107, -1421, -247, -951, -398, 961, -1508, -725, 448, -1065, 677, -1275,
    -1103, 430, 555, 843, -1251, 871, 1550, 105, 422, 587, 177, -235, -291, -460,
    1574, 1653, -246, 778, 1159, -147, -777, 1483, -602, 1119, -1590, 644, -872,
    349, 418, 329, -156, -75, 817, 1097, 603, 610, 1322, -1285, -1465, 384, -1215,
    -136, 1218, -1335, -874, 220, -1187, -1659, -1185, -1530, -1278, 794, -1510,
    -854, -870, 478, -108, -308, 996, 991, 958, -1460, 1522, 1628,
]


def to_signed16(x: int) -> int:
    x &= 0xFFFF
    return x - 0x10000 if x >= 0x8000 else x


def montgomery_reduce(a: int) -> int:
    """输入 |a| < q*2^15，输出 ≡ a * 2^-16 (mod q)，范围 (-q, q)"""
    t = to_signed16((a * QINV) & 0xFFFF)
    return (a - t * Q) >> 16


def barrett_reduce(a: int) -> int:
    """把 a 归约到 (-q/2, q/2]"""
    v = ((1 << 26) + Q // 2) // Q
    t = (v * a + (1 << 25)) >> 26
    return a - t * Q


def fqmul(a: int, b: int) -> int:
    return montgomery_reduce(a * b)


def ct_butterfly(a: int, b: int, zeta: int) -> tuple[int, int]:
    """Cooley-Tukey 蝶形（正向 NTT 用）"""
    t = fqmul(zeta, b)
    return a + t, a - t


def gs_butterfly(a: int, b: int, zeta: int) -> tuple[int, int]:
    """Gentleman-Sande 蝶形（逆向 NTT 用）"""
    t = a
    a2 = barrett_reduce(t + b)
    b2 = fqmul(zeta, b - t)
    return a2, b2


def ntt(poly: list[int]) -> list[int]:
    """256 点前向 NTT。**只做 7 层**（len 从 128 降到 2）。"""
    a = list(poly)
    k = 1
    length = 128
    while length >= 2:
        start = 0
        while start < 256:
            zeta = ZETAS[k]
            k += 1
            for j in range(start, start + length):
                a[j], a[j + length] = ct_butterfly(a[j], a[j + length], zeta)
            start += 2 * length
        length >>= 1
    return [barrett_reduce(x) for x in a]


def invntt(poly: list[int]) -> list[int]:
    """256 点逆 NTT（含最后的 f 缩放）"""
    a = list(poly)
    k = 127
    length = 2
    while length <= 128:
        start = 0
        while start < 256:
            zeta = ZETAS[k]        # 参考实现是 zetas[k--]，后减
            k -= 1
            for j in range(start, start + length):
                a[j], a[j + length] = gs_butterfly(a[j], a[j + length], zeta)
            start += 2 * length
        length <<= 1
    return [fqmul(x, F) for x in a]


def basemul(a0: int, a1: int, b0: int, b1: int, zeta: int) -> tuple[int, int]:
    """NTT 域基乘：(a0 + a1·x)(b0 + b1·x) mod (x² − ζ)，全程 Montgomery 域

    ML-KEM 的 NTT 只做 7 层，变换结果是 128 个一次多项式，所以"逐点乘"
    实际是每对系数在 Z_q[x]/(x² ∓ ζ) 上的乘法。
    """
    r0 = fqmul(fqmul(a1, b1), zeta) + fqmul(a0, b0)
    r1 = fqmul(a0, b1) + fqmul(a1, b0)
    return r0, r1


def compress(x: int, d: int) -> int:
    """Compress_d：把系数从 [0, q) 压到 [0, 2^d)

    整数形式 floor(((x << d) + q/2) / q)，与 FIPS 203 的
    round(2^d/q · x) mod 2^d 等价（等价性由 mlkem_oracle.py 穷举验证）。
    """
    u = x % Q
    return (((u << d) + Q // 2) // Q) & ((1 << d) - 1)


def decompress(y: int, d: int) -> int:
    """Decompress_d：把 [0, 2^d) 的值还原回 [0, q)"""
    return (Q * y + (1 << (d - 1))) >> d


def cbd2(rand: int) -> list[int]:
    """η = 2 的中心二项分布采样：4 字节 → 8 个系数（位并行写法）"""
    mask = 0x55555555
    d = (rand & mask) + ((rand >> 1) & mask)
    out = []
    for i in range(8):
        a = (d >> (4 * i)) & 3
        b = (d >> (4 * i + 2)) & 3
        out.append(a - b)
    return out


def cbd3(rand: int) -> list[int]:
    """η = 3 的中心二项分布采样：3 字节 → 4 个系数（位并行写法）"""
    mask = 0x00249249
    d = (rand & mask) + ((rand >> 1) & mask) + ((rand >> 2) & mask)
    out = []
    for i in range(4):
        a = (d >> (6 * i)) & 7
        b = (d >> (6 * i + 3)) & 7
        out.append(a - b)
    return out


def rej_pair(b0: int, b1: int, b2: int) -> tuple[int, int]:
    """SampleNTT 的取候选一步：3 字节 → 两个 12 位候选（是否小于 q 由调用方判）"""
    d1 = b0 | ((b1 & 0x0F) << 8)
    d2 = (b1 >> 4) | (b2 << 4)
    return d1, d2


def encode12(c0: int, c1: int) -> tuple[int, int, int]:
    """ByteEncode12：两个系数（先折回 [0, q)）打进 3 个字节"""
    t0, t1 = c0 % Q, c1 % Q
    return (t0 & 0xFF, ((t0 >> 8) | (t1 << 4)) & 0xFF, (t1 >> 4) & 0xFF)


def decode12(b0: int, b1: int, b2: int) -> tuple[int, int]:
    """ByteDecode12：3 个字节还原两个 12 位系数"""
    return (b0 | ((b1 & 0x0F) << 8), (b1 >> 4) | (b2 << 4))


def keccak_f1600(state: list[int]) -> list[int]:
    """Keccak-f[1600]，输入输出都是 25 个 64-bit lane。

    独立实现，用来给 RTL 的置换核对拍；SHAKE 层面另有 hashlib 交叉验证。
    """
    RC = [
        0x0000000000000001, 0x0000000000008082, 0x800000000000808A, 0x8000000080008000,
        0x000000000000808B, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
        0x000000000000008A, 0x0000000000000088, 0x0000000080008009, 0x000000008000000A,
        0x000000008000808B, 0x800000000000008B, 0x8000000000008089, 0x8000000000008003,
        0x8000000000008002, 0x8000000000000080, 0x000000000000800A, 0x800000008000000A,
        0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008,
    ]
    R = [[0, 36, 3, 41, 18], [1, 44, 10, 45, 2], [62, 6, 43, 15, 61],
         [28, 55, 25, 21, 56], [27, 20, 39, 8, 14]]
    M = (1 << 64) - 1

    def rol(x, n):
        return ((x << n) | (x >> (64 - n))) & M

    A = [[state[x + 5 * y] for y in range(5)] for x in range(5)]
    for rnd in range(24):
        C = [A[x][0] ^ A[x][1] ^ A[x][2] ^ A[x][3] ^ A[x][4] for x in range(5)]
        D = [C[(x - 1) % 5] ^ rol(C[(x + 1) % 5], 1) for x in range(5)]
        for x in range(5):
            for y in range(5):
                A[x][y] ^= D[x]
        B = [[0] * 5 for _ in range(5)]
        for x in range(5):
            for y in range(5):
                B[y][(2 * x + 3 * y) % 5] = rol(A[x][y], R[x][y])
        for x in range(5):
            for y in range(5):
                A[x][y] = B[x][y] ^ ((~B[(x + 1) % 5][y]) & B[(x + 2) % 5][y] & M)
        A[0][0] ^= RC[rnd]
    return [A[x][y] for y in range(5) for x in range(5)]


def shake128(msg: bytes, outlen: int) -> bytes:
    """交叉验证用：直接白嫖标准库"""
    return hashlib.shake_128(msg).digest(outlen)


def shake256(msg: bytes, outlen: int) -> bytes:
    return hashlib.shake_256(msg).digest(outlen)


def _self_test() -> None:
    """自检：NTT/INTT 往返必须还原原多项式"""
    import random
    random.seed(20260729)
    for _ in range(20):
        p = [random.randrange(-Q // 2, Q // 2) for _ in range(256)]
        r = invntt(ntt(list(p)))
        # ⚠️ 往返**不是**恒等：参考实现的 invntt 里 f = mont^2/128，
        # 于是 invntt(ntt(x)) ≡ x · 2^16 (mod q)。这一条最容易被误判成 bug，
        # 独立重写一遍模型的价值就在于把这类约定摊开。
        for a, b in zip(p, r):
            assert (a * MONT - b) % Q == 0, f"NTT 往返不符 x·mont: {a} vs {b}"
    # Montgomery 约减的定义式
    for a in (0, 1, 1234, -1234, Q * 100, -Q * 100):
        assert (montgomery_reduce(a) * (1 << 16) - a) % Q == 0
    # Keccak：全零态置换的已知首 lane
    st = keccak_f1600([0] * 25)
    assert st[0] == 0xF1258F7940E1DDE7, f"keccak 首 lane 不对: {st[0]:016x}"
    print("ref_model 自检通过：NTT 往返、Montgomery 定义式、Keccak-f[1600]")


if __name__ == "__main__":
    _self_test()
