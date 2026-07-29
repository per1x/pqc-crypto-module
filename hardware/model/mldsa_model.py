#!/usr/bin/env python3
"""ML-DSA 的独立 Python 参考模型

与 ref_model.py（ML-KEM 侧）同样的定位：给 RTL 提供分层黄金向量，并用一份
独立重写的实现去证伪对 C 实现的理解偏差。

参数取 FIPS 204：q = 8380417（23 位），系数用 32 位有符号承载，
Montgomery 约减以 R = 2³² 为基 —— 与 ML-KEM 侧的 R = 2¹⁶ 不同，
两套算术不能混用，所以 RTL 里也分成 mlkem_ 与 mldsa_ 两组模块。

覆盖硬件最关心的几层：
  L0 算子：Montgomery 约减、reduce32、caddq
  L1 蝶形：CT / GS 蝶形
  L2 模块：256 点 NTT / INTT（**完整 8 层**，与 ML-KEM 的 7 层不同）
  L2 模块：Power2Round / Decompose / MakeHint / UseHint / 无穷范数检查
  L2 模块：拒绝采样（均匀分布与 [−η, η] 有界分布）
"""
from __future__ import annotations

Q = 8380417
QINV = 58728449          # q⁻¹ mod 2³²
D = 13                   # Power2Round 的位数
N = 256
ROOT = 1753              # q 的 512 次本原单位根

GAMMA2_88 = (Q - 1) // 88    # ML-DSA-44
GAMMA2_32 = (Q - 1) // 32    # ML-DSA-65 / 87

# 参数集：k, l, eta, gamma2, tau, beta 中硬件用得到的几项
PARAMS = {
    "ML-DSA-44": {"k": 4, "l": 4, "eta": 2, "gamma2": GAMMA2_88},
    "ML-DSA-65": {"k": 6, "l": 5, "eta": 4, "gamma2": GAMMA2_32},
    "ML-DSA-87": {"k": 8, "l": 7, "eta": 2, "gamma2": GAMMA2_32},
}


def _brv8(i: int) -> int:
    return int(f"{i:08b}"[::-1], 2)


def _build_zetas() -> list[int]:
    """zetas[i] = ROOT^brv8(i) · 2³² mod q，折算到 (−q/2, q/2]

    下标 0 在正逆变换里都取不到（正变换从 ++k 开始，逆变换从 --k 开始），
    与参考实现一样置 0，避免出现一个"看着像旋转因子但永远用不上"的常数。
    """
    out = [0]
    for i in range(1, 256):
        v = pow(ROOT, _brv8(i), Q) * (1 << 32) % Q
        out.append(v - Q if v > Q // 2 else v)
    return out


ZETAS = _build_zetas()


def to_signed32(x: int) -> int:
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x >= (1 << 31) else x


def montgomery_reduce(a: int) -> int:
    """输入 |a| < q·2³¹，输出 ≡ a·2⁻³² (mod q)，范围 (−q, q)

    与 C 实现逐位等价的关键是 `(int32_t)a` 这一步截断：先取 a 的低 32 位当
    有符号数，乘 QINV 后再截断到 32 位，否则差值不被 2³² 整除。
    """
    t = to_signed32(to_signed32(a) * QINV)
    return (a - t * Q) >> 32


def reduce32(a: int) -> int:
    """把 a 归约到 [−6283009, 6283008]

    定义域是 |a| ≤ 2³¹ − 2²² − 1：再大一点 `a + 2²²` 就会越出 32 位有符号，
    结果不再与 a 同余。这里刻意**照 32 位截断语义实现**（而不是用 Python 的
    大整数），这样模型在整个 32 位输入域上都与 C 参考实现和 RTL 逐位一致，
    定义域之外的行为也能被比对，而不是模型算一套、硬件算另一套。
    """
    t = to_signed32(a + (1 << 22)) >> 23
    return to_signed32(a - t * Q)


def caddq(a: int) -> int:
    """负数加一个 q，把系数折回 [0, q)"""
    return a + Q if a < 0 else a


def fqmul(a: int, b: int) -> int:
    return montgomery_reduce(a * b)


def ct_butterfly(a: int, b: int, zeta: int) -> tuple[int, int]:
    """Cooley-Tukey 蝶形（正向 NTT 用）"""
    t = fqmul(zeta, b)
    return a + t, a - t


def gs_butterfly(a: int, b: int, zeta: int) -> tuple[int, int]:
    """Gentleman-Sande 蝶形（逆 NTT 用）"""
    return a + b, fqmul(zeta, a - b)


def ntt(poly: list[int]) -> list[int]:
    """256 点前向 NTT，**完整 8 层**（len 从 128 降到 1）"""
    a = list(poly)
    k = 0
    length = 128
    while length >= 1:
        start = 0
        while start < N:
            k += 1
            zeta = ZETAS[k]
            for j in range(start, start + length):
                t = fqmul(zeta, a[j + length])
                a[j + length] = a[j] - t
                a[j] = a[j] + t
            start += 2 * length
        length >>= 1
    return a


def invntt_tomont(poly: list[int]) -> list[int]:
    """256 点逆 NTT，末尾乘 f = mont²/256

    因此 invntt_tomont(ntt(x)) ≡ x·2³² (mod q)，**不是恒等** ——
    与 ML-KEM 侧同类约定一样，这一条最容易被误判成缺陷。
    """
    f = 41978                     # mont²/256
    a = list(poly)
    k = 256
    length = 1
    while length < N:
        start = 0
        while start < N:
            k -= 1
            zeta = -ZETAS[k]
            for j in range(start, start + length):
                t = a[j]
                a[j] = t + a[j + length]
                a[j + length] = fqmul(zeta, t - a[j + length])
            start += 2 * length
        length <<= 1
    return [fqmul(f, x) for x in a]


def power2round(a: int) -> tuple[int, int]:
    """a ≡ a1·2^D + a0 (mod q)，a0 落在 (−2^(D−1), 2^(D−1)]；返回 (a0, a1)"""
    a1 = (a + (1 << (D - 1)) - 1) >> D
    return a - (a1 << D), a1


def decompose(a: int, gamma2: int) -> tuple[int, int]:
    """a ≡ a1·2γ₂ + a0 (mod q)，返回 (a0, a1)

    除以 2γ₂ 同样用"乘倒数再右移"代替，两个 γ₂ 各有一组常数。
    """
    a1 = (a + 127) >> 7
    if gamma2 == GAMMA2_32:
        a1 = (a1 * 1025 + (1 << 21)) >> 22
        a1 &= 15
    else:
        a1 = (a1 * 11275 + (1 << 23)) >> 24
        a1 ^= ((43 - a1) >> 31) & a1        # a1 == 44 时钳到 0
    a0 = a - a1 * 2 * gamma2
    a0 -= (((Q - 1) // 2 - a0) >> 31) & Q
    return a0, a1


def make_hint(a0: int, a1: int, gamma2: int) -> int:
    """低位是否会导致高位进位"""
    if a0 > gamma2 or a0 < -gamma2 or (a0 == -gamma2 and a1 != 0):
        return 1
    return 0


def use_hint(a: int, hint: int, gamma2: int) -> int:
    """按提示还原高位"""
    a0, a1 = decompose(a, gamma2)
    if hint == 0:
        return a1
    if gamma2 == GAMMA2_32:
        return (a1 + 1) & 15 if a0 > 0 else (a1 - 1) & 15
    if a0 > 0:
        return 0 if a1 == 43 else a1 + 1
    return 43 if a1 == 0 else a1 - 1


def chknorm(a: int, bound: int) -> int:
    """无穷范数检查：|a| ≥ bound 时返回 1

    参考实现刻意不用条件分支取绝对值（`t = a >> 31; t = a − (t & 2a)`），
    因为这一步会作用在私密系数上。
    """
    t = a >> 31
    t = a - (t & (2 * a))
    return 1 if t >= bound else 0


def rej_uniform_coeff(b0: int, b1: int, b2: int) -> tuple[int, int]:
    """均匀拒绝采样的取候选一步：3 字节 → 23 位候选，返回 (值, 是否接受)"""
    t = (b0 | (b1 << 8) | (b2 << 16)) & 0x7FFFFF
    return t, 1 if t < Q else 0


def rej_eta_coeff(nibble: int, eta: int) -> tuple[int, int]:
    """有界拒绝采样：4 位 → [−η, η] 的系数，返回 (值, 是否接受)

    η = 2 时先把 [0, 15) 折到 [0, 5)（用乘倒数代替对 5 取模），再算 2 − t。
    """
    if eta == 2:
        if nibble < 15:
            t = nibble - (205 * nibble >> 10) * 5
            return 2 - t, 1
        return 0, 0
    if nibble < 9:
        return 4 - nibble, 1
    return 0, 0


def _self_test() -> None:
    """自检：旋转因子表、NTT 往返、各算子的定义式"""
    import random

    # 旋转因子表与参考实现的已知取值一致
    assert ZETAS[1] == 25847 and ZETAS[2] == -2608894 and ZETAS[255] == 1976782, \
        "zeta 表与参考实现不一致"

    random.seed(20260730)
    for _ in range(5):
        p = [random.randrange(-Q // 2, Q // 2) for _ in range(N)]
        r = invntt_tomont(ntt(list(p)))
        mont = (1 << 32) % Q
        for a, b in zip(p, r):
            assert (a * mont - b) % Q == 0, "NTT 往返不符 x·2³²"

    for a in (0, 1, -1, 12345, -12345, Q * 1000, -(Q * 1000)):
        assert (montgomery_reduce(a) * (1 << 32) - a) % Q == 0
        assert -Q < montgomery_reduce(a) < Q

    for _ in range(2000):
        a = random.randrange(Q)
        a0, a1 = power2round(a)
        assert (a1 * (1 << D) + a0 - a) % Q == 0
        assert -(1 << (D - 1)) < a0 <= (1 << (D - 1))
        for g2 in (GAMMA2_88, GAMMA2_32):
            r0, r1 = decompose(a, g2)
            assert (r1 * 2 * g2 + r0 - a) % Q == 0
            assert -g2 < r0 <= g2 or a1 == 0
            # 无提示时 UseHint 必须原样还原高位
            assert use_hint(a, 0, g2) == r1
    print("mldsa_model 自检通过：zeta 表、NTT 往返、Power2Round/Decompose 定义式")


if __name__ == "__main__":
    _self_test()
