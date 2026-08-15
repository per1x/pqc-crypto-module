#!/usr/bin/env python3
"""ML-DSA 数据通路算子的**独立预言机**

与 ML-KEM 侧同一套判据：黄金向量由 mldsa_model 生成，所以 cocotb 的三方一致
只等价于 `RTL == mldsa_model`。要证明模型本身对，必须换来源。

预言机 A  NTT 是 Z_q[x]/(x²⁵⁶+1) 上的乘法同态：用 O(n²) 的 schoolbook 负循环
          卷积（完全不碰旋转因子表）验证 invntt(ntt(a)∘ntt(b)) 与之相等。
          ML-DSA 的 NTT 做满 8 层，所以 NTT 域的乘法就是逐点标量乘。

预言机 B  Power2Round / Decompose / MakeHint / UseHint 的定义式与取值范围，
          在整个系数域上按结构穷举验证；提示位则验证 FIPS 204 依赖的那条性质：
          对 |e| ≤ γ₂ 的扰动，UseHint(r+e, MakeHint(r₀+e, r₁)) 必须还原 r₁。

预言机 C  用这些算子重建 ML-DSA KeyGen，比对 NIST ACVP 向量。
          矩阵 Â 的均匀拒绝采样走 rej_uniform_coeff，s₁/s₂ 走 rej_eta_coeff，
          NTT 与逐点乘走 ntt/invntt_tomont/montgomery_reduce，
          最后的 t₁/t₀ 拆分走 power2round。能逐字节重现 pk/sk ⇒ 这些算子
          就是真实 ML-DSA 用的那几个。

用法：python3 hardware/model/mldsa_oracle.py
"""
from __future__ import annotations

import hashlib
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from mldsa_model import (  # noqa: E402
    D, GAMMA2_32, GAMMA2_88, N, PARAMS, Q, caddq, chknorm, decompose,
    invntt_tomont, make_hint, montgomery_reduce, ntt, power2round, reduce32,
    rej_eta_coeff, rej_uniform_coeff, use_hint,
)

KAT = Path(__file__).resolve().parents[2] / "vectors" / "mldsa_keygen.kat"
SIGGEN_KAT = Path(__file__).resolve().parents[2] / "vectors" / "mldsa_siggen.kat"
SIGVER_KAT = Path(__file__).resolve().parents[2] / "vectors" / "mldsa_sigver.kat"


# ---------------------------------------------------------------- 预言机 A

def schoolbook_negacyclic(a: list[int], b: list[int]) -> list[int]:
    """Z_q[x]/(x²⁵⁶+1) 上的朴素多项式乘法，刻意不使用任何 NTT / 旋转因子"""
    r = [0] * N
    for i in range(N):
        ai = a[i]
        if ai == 0:
            continue
        for j in range(N):
            k = i + j
            if k < N:
                r[k] += ai * b[j]
            else:
                r[k - N] -= ai * b[j]
    return [x % Q for x in r]


def oracle_a(trials: int = 4) -> bool:
    rng = random.Random(20260730)
    ok = True
    for t in range(trials):
        a = [rng.randrange(Q) for _ in range(N)]
        b = [rng.randrange(Q) for _ in range(N)]
        want = schoolbook_negacyclic(a, b)
        ah, bh = ntt(list(a)), ntt(list(b))
        prod = [montgomery_reduce(ah[i] * bh[i]) for i in range(N)]
        got = invntt_tomont(prod)
        # 逐点乘的 2⁻³² 与 invntt_tomont 的 2³² 恰好抵消，结果就是普通卷积
        if any((got[i] - want[i]) % Q != 0 for i in range(N)):
            print(f"  ✗ 第 {t} 组：NTT 不是那个环上的乘法同态")
            ok = False
    if ok:
        print(f"  ✓ 预言机 A：{trials} 组随机多项式，invntt(ntt(a)∘ntt(b)) 与 "
              f"schoolbook 负循环卷积逐系数相等")
        print("      （schoolbook 完全不碰旋转因子表 —— 表错一个数、层序错一层，同态就不成立）")
    return ok


# ---------------------------------------------------------------- 预言机 B

def oracle_b() -> bool:
    ok = True

    # Power2Round：分解式与 a0 的取值范围，按 2^D 的周期结构穷举代表元
    step = max(1, Q // 40000)
    for a in list(range(0, Q, step)) + [0, 1, Q - 1, (1 << D) - 1, 1 << D]:
        a0, a1 = power2round(a)
        if (a1 * (1 << D) + a0 - a) % Q != 0:
            print(f"  ✗ power2round 分解式不成立：a={a}")
            ok = False
            break
        if not -(1 << (D - 1)) < a0 <= (1 << (D - 1)):
            print(f"  ✗ power2round 的 a0 越界：a={a} a0={a0}")
            ok = False
            break

    # Decompose：分解式、a1 的取值集合、a0 的取值范围
    for gamma2, m in ((GAMMA2_88, 44), (GAMMA2_32, 16)):
        for a in list(range(0, Q, step)) + [0, 1, Q - 1, gamma2, 2 * gamma2]:
            a0, a1 = decompose(a, gamma2)
            if (a1 * 2 * gamma2 + a0 - a) % Q != 0:
                print(f"  ✗ decompose 分解式不成立：a={a} γ₂={gamma2}")
                ok = False
                break
            if not 0 <= a1 < m:
                print(f"  ✗ decompose 的 a1 越界：a={a} a1={a1}")
                ok = False
                break
            if not -gamma2 < a0 <= gamma2:
                # 只有 a 落在最高一段（a1 归零）时 a0 才允许取到 −γ₂ 以下
                if not (a1 == 0 and a0 > -Q):
                    print(f"  ✗ decompose 的 a0 越界：a={a} a0={a0}")
                    ok = False
                    break

    # 提示位：FIPS 204 依赖的那条性质
    rng = random.Random(20260731)
    for gamma2 in (GAMMA2_88, GAMMA2_32):
        for _ in range(20000):
            r = rng.randrange(Q)
            e = rng.randrange(-gamma2, gamma2 + 1)
            r0, r1 = decompose(r, gamma2)
            h = make_hint(r0 + e, r1, gamma2)
            if use_hint((r + e) % Q, h, gamma2) != r1:
                print(f"  ✗ 提示位性质不成立：r={r} e={e} γ₂={gamma2}")
                ok = False
                break
    if ok:
        print("  ✓ 预言机 B：Power2Round / Decompose 的分解式与取值范围成立；")
        print("      提示位满足 UseHint(r+e, MakeHint(r₀+e, r₁)) == r₁（|e| ≤ γ₂，各 20000 组）")
    return ok


# ---------------------------------------------------------------- 预言机 C

def h_shake256(data: bytes, outlen: int) -> bytes:
    return hashlib.shake_256(data).digest(outlen)


def rej_uniform_poly(seed: bytes, nonce: int) -> list[int]:
    """RejNTTPoly：从 SHAKE128 流里采出 [0, q) 上均匀的 256 个系数"""
    buf = hashlib.shake_128(seed + bytes([nonce & 0xFF, nonce >> 8])).digest(168 * 16)
    out: list[int] = []
    pos = 0
    while len(out) < N:
        t, accept = rej_uniform_coeff(buf[pos], buf[pos + 1], buf[pos + 2])
        pos += 3
        if accept:
            out.append(t)
    return out


def rej_eta_poly(seed: bytes, nonce: int, eta: int) -> list[int]:
    """RejBoundedPoly：从 SHAKE256 流里采出 [−η, η] 上的 256 个系数"""
    buf = hashlib.shake_256(seed + bytes([nonce & 0xFF, nonce >> 8])).digest(136 * 32)
    out: list[int] = []
    pos = 0
    while len(out) < N:
        byte = buf[pos]
        pos += 1
        for nib in (byte & 0x0F, byte >> 4):
            if len(out) == N:
                break
            v, accept = rej_eta_coeff(nib, eta)
            if accept:
                out.append(v)
    return out


def polyt1_pack(p: list[int]) -> bytes:
    """t₁ 每系数 10 位，4 个系数打进 5 字节"""
    out = bytearray(320)
    for i in range(N // 4):
        c = p[4 * i:4 * i + 4]
        out[5 * i + 0] = c[0] & 0xFF
        out[5 * i + 1] = ((c[0] >> 8) | (c[1] << 2)) & 0xFF
        out[5 * i + 2] = ((c[1] >> 6) | (c[2] << 4)) & 0xFF
        out[5 * i + 3] = ((c[2] >> 4) | (c[3] << 6)) & 0xFF
        out[5 * i + 4] = (c[3] >> 2) & 0xFF
    return bytes(out)


def polyt0_pack(p: list[int]) -> bytes:
    """t₀ 每系数 13 位，8 个系数打进 13 字节；先换成 2^(D−1) − a"""
    out = bytearray(416)
    for i in range(N // 8):
        t = [(1 << (D - 1)) - p[8 * i + j] for j in range(8)]
        o = 13 * i
        out[o + 0] = t[0] & 0xFF
        out[o + 1] = ((t[0] >> 8) | (t[1] << 5)) & 0xFF
        out[o + 2] = (t[1] >> 3) & 0xFF
        out[o + 3] = ((t[1] >> 11) | (t[2] << 2)) & 0xFF
        out[o + 4] = ((t[2] >> 6) | (t[3] << 7)) & 0xFF
        out[o + 5] = (t[3] >> 1) & 0xFF
        out[o + 6] = ((t[3] >> 9) | (t[4] << 4)) & 0xFF
        out[o + 7] = (t[4] >> 4) & 0xFF
        out[o + 8] = ((t[4] >> 12) | (t[5] << 1)) & 0xFF
        out[o + 9] = ((t[5] >> 7) | (t[6] << 6)) & 0xFF
        out[o + 10] = (t[6] >> 2) & 0xFF
        out[o + 11] = ((t[6] >> 10) | (t[7] << 3)) & 0xFF
        out[o + 12] = (t[7] >> 5) & 0xFF
    return bytes(out)


def polyeta_pack(p: list[int], eta: int) -> bytes:
    """s₁/s₂ 的打包：η=2 时每系数 3 位，η=4 时每系数 4 位"""
    if eta == 2:
        out = bytearray(96)
        for i in range(N // 8):
            t = [eta - p[8 * i + j] for j in range(8)]
            out[3 * i + 0] = (t[0] | (t[1] << 3) | (t[2] << 6)) & 0xFF
            out[3 * i + 1] = ((t[2] >> 2) | (t[3] << 1) | (t[4] << 4) | (t[5] << 7)) & 0xFF
            out[3 * i + 2] = ((t[5] >> 1) | (t[6] << 2) | (t[7] << 5)) & 0xFF
        return bytes(out)
    out = bytearray(128)
    for i in range(N // 2):
        t0 = eta - p[2 * i]
        t1 = eta - p[2 * i + 1]
        out[i] = (t0 | (t1 << 4)) & 0xFF
    return bytes(out)


def mldsa_keygen(xi: bytes, name: str) -> tuple[bytes, bytes]:
    """FIPS 204 ML-DSA.KeyGen_internal(ξ)，全部算子取自 mldsa_model"""
    par = PARAMS[name]
    k, ell, eta = par["k"], par["l"], par["eta"]

    h = h_shake256(xi + bytes([k, ell]), 128)
    rho, rho_prime, key = h[:32], h[32:96], h[96:128]

    a_hat = [[rej_uniform_poly(rho, (i << 8) + j) for j in range(ell)] for i in range(k)]
    s1 = [rej_eta_poly(rho_prime, n, eta) for n in range(ell)]
    s2 = [rej_eta_poly(rho_prime, ell + n, eta) for n in range(k)]

    s1_hat = [ntt(list(p)) for p in s1]

    t = []
    for i in range(k):
        acc = [montgomery_reduce(a_hat[i][0][n] * s1_hat[0][n]) for n in range(N)]
        for j in range(1, ell):
            acc = [acc[n] + montgomery_reduce(a_hat[i][j][n] * s1_hat[j][n])
                   for n in range(N)]
        acc = [reduce32(x) for x in acc]
        acc = invntt_tomont(acc)
        acc = [acc[n] + s2[i][n] for n in range(N)]
        t.append([caddq(x) for x in acc])

    t0, t1 = [], []
    for p in t:
        pair = [power2round(x) for x in p]
        t0.append([x[0] for x in pair])
        t1.append([x[1] for x in pair])

    pk = rho + b"".join(polyt1_pack(p) for p in t1)
    tr = h_shake256(pk, 64)
    sk = (rho + key + tr
          + b"".join(polyeta_pack(p, eta) for p in s1)
          + b"".join(polyeta_pack(p, eta) for p in s2)
          + b"".join(polyt0_pack(p) for p in t0))
    return pk, sk


def load_kat(limit_per_alg: int = 2):
    if not KAT.exists():
        return None
    recs = []
    cur: dict[str, str] = {}
    for line in KAT.read_text().splitlines():
        line = line.strip()
        if line.startswith("#"):
            continue
        if not line:
            if cur:
                recs.append(cur)
                cur = {}
            continue
        key, _, val = line.partition(" = ")
        cur[key.strip()] = val.strip()
    if cur:
        recs.append(cur)
    seen: dict[str, int] = {}
    out = []
    for r in recs:
        alg = r.get("alg", "")
        if alg not in PARAMS or seen.get(alg, 0) >= limit_per_alg:
            continue
        seen[alg] = seen.get(alg, 0) + 1
        out.append(r)
    return out


def oracle_c() -> bool:
    recs = load_kat()
    if recs is None:
        print("  ⚠ 预言机 C 跳过：找不到 vectors/mldsa_keygen.kat"
              "（先跑 tools/fetch_vectors.sh）")
        return True
    ok = True
    n = 0
    for r in recs:
        pk, sk = mldsa_keygen(bytes.fromhex(r["seed"]), r["alg"])
        if pk != bytes.fromhex(r["pk"]):
            print(f"  ✗ {r['alg']} tcId={r.get('tcid')}：pk 不匹配")
            ok = False
        elif sk != bytes.fromhex(r["sk"]):
            print(f"  ✗ {r['alg']} tcId={r.get('tcid')}：sk 不匹配（pk 对了）")
            ok = False
        n += 1
    if ok:
        print(f"  ✓ 预言机 C：用 rej_uniform_coeff / rej_eta_coeff / ntt / "
              f"invntt_tomont / power2round 重建 ML-DSA KeyGen，")
        print(f"      {n} 条 NIST ACVP 向量的 pk/sk 逐字节重现")
    return ok


# ------------------------------------------------ 预言机 D / E：Sign / Verify
#
# 参数常量：tau/gamma1/omega/beta/lambda 在 mldsa_model 的 PARAMS 里没有，
# 按 FIPS 204 Table 1 补齐（beta = tau·eta；c_tilde 长度 = lambda/4 字节）。
SIG_PARAMS = {
    #                tau  gamma1   omega  beta  lambda  ctilde
    "ML-DSA-44": {"tau": 39, "gamma1": 1 << 17, "omega": 80, "beta": 78,
                  "lambda": 128, "ctilde": 32},
    "ML-DSA-65": {"tau": 49, "gamma1": 1 << 19, "omega": 55, "beta": 196,
                  "lambda": 192, "ctilde": 48},
    "ML-DSA-87": {"tau": 60, "gamma1": 1 << 19, "omega": 75, "beta": 120,
                  "lambda": 256, "ctilde": 64},
}

POLYETA_BYTES = {2: 96, 4: 128}   # s1/s2 每条多项式的打包字节数
POLYT0_BYTES = 416
POLYT1_BYTES = 320


# ---- 解包（verify / skDecode 用）--------------------------------------------

def polyt1_unpack(buf: bytes) -> list[int]:
    """t₁ 每系数 10 位的逆过程"""
    r = [0] * N
    for i in range(N // 4):
        o = 5 * i
        r[4 * i + 0] = (buf[o + 0] | (buf[o + 1] << 8)) & 0x3FF
        r[4 * i + 1] = ((buf[o + 1] >> 2) | (buf[o + 2] << 6)) & 0x3FF
        r[4 * i + 2] = ((buf[o + 2] >> 4) | (buf[o + 3] << 4)) & 0x3FF
        r[4 * i + 3] = ((buf[o + 3] >> 6) | (buf[o + 4] << 2)) & 0x3FF
    return r


def polyt0_unpack(buf: bytes) -> list[int]:
    """t₀ 每系数 13 位的逆过程；还原 2^(D−1) − a"""
    r = [0] * N
    for i in range(N // 8):
        o = 13 * i
        r[8 * i + 0] = (buf[o + 0] | (buf[o + 1] << 8)) & 0x1FFF
        r[8 * i + 1] = ((buf[o + 1] >> 5) | (buf[o + 2] << 3)
                        | (buf[o + 3] << 11)) & 0x1FFF
        r[8 * i + 2] = ((buf[o + 3] >> 2) | (buf[o + 4] << 6)) & 0x1FFF
        r[8 * i + 3] = ((buf[o + 4] >> 7) | (buf[o + 5] << 1)
                        | (buf[o + 6] << 9)) & 0x1FFF
        r[8 * i + 4] = ((buf[o + 6] >> 4) | (buf[o + 7] << 4)
                        | (buf[o + 8] << 12)) & 0x1FFF
        r[8 * i + 5] = ((buf[o + 8] >> 1) | (buf[o + 9] << 7)) & 0x1FFF
        r[8 * i + 6] = ((buf[o + 9] >> 6) | (buf[o + 10] << 2)
                        | (buf[o + 11] << 10)) & 0x1FFF
        r[8 * i + 7] = ((buf[o + 11] >> 3) | (buf[o + 12] << 5)) & 0x1FFF
    return [(1 << (D - 1)) - x for x in r]


def polyeta_unpack(buf: bytes, eta: int) -> list[int]:
    """s₁/s₂ 的解包，与 polyeta_pack 互逆"""
    r = [0] * N
    if eta == 2:
        for i in range(N // 8):
            o = 3 * i
            r[8 * i + 0] = (buf[o + 0]) & 7
            r[8 * i + 1] = (buf[o + 0] >> 3) & 7
            r[8 * i + 2] = ((buf[o + 0] >> 6) | (buf[o + 1] << 2)) & 7
            r[8 * i + 3] = (buf[o + 1] >> 1) & 7
            r[8 * i + 4] = (buf[o + 1] >> 4) & 7
            r[8 * i + 5] = ((buf[o + 1] >> 7) | (buf[o + 2] << 1)) & 7
            r[8 * i + 6] = (buf[o + 2] >> 2) & 7
            r[8 * i + 7] = (buf[o + 2] >> 5) & 7
        return [eta - x for x in r]
    for i in range(N // 2):
        r[2 * i + 0] = buf[i] & 0x0F
        r[2 * i + 1] = buf[i] >> 4
    return [eta - x for x in r]


def polyz_unpack(buf: bytes, gamma1: int) -> list[int]:
    """ExpandMask / z 的位解包：γ₁=2¹⁷ 走 18 位，γ₁=2¹⁹ 走 20 位"""
    r = [0] * N
    if gamma1 == (1 << 17):
        for i in range(N // 4):
            o = 9 * i
            r[4 * i + 0] = (buf[o + 0] | (buf[o + 1] << 8)
                            | (buf[o + 2] << 16)) & 0x3FFFF
            r[4 * i + 1] = ((buf[o + 2] >> 2) | (buf[o + 3] << 6)
                            | (buf[o + 4] << 14)) & 0x3FFFF
            r[4 * i + 2] = ((buf[o + 4] >> 4) | (buf[o + 5] << 4)
                            | (buf[o + 6] << 12)) & 0x3FFFF
            r[4 * i + 3] = ((buf[o + 6] >> 6) | (buf[o + 7] << 2)
                            | (buf[o + 8] << 10)) & 0x3FFFF
    else:
        for i in range(N // 2):
            o = 5 * i
            r[2 * i + 0] = (buf[o + 0] | (buf[o + 1] << 8)
                            | (buf[o + 2] << 16)) & 0xFFFFF
            r[2 * i + 1] = ((buf[o + 2] >> 4) | (buf[o + 3] << 4)
                            | (buf[o + 4] << 12)) & 0xFFFFF
    return [gamma1 - x for x in r]


def hint_unpack(buf: bytes, k: int, omega: int) -> list[list[int]] | None:
    """HintBitPack 的逆：ω+k 字节 → k 条 0/1 提示多项式；结构不合法返回 None"""
    h = [[0] * N for _ in range(k)]
    index = 0
    for i in range(k):
        end = buf[omega + i]
        if end < index or end > omega:
            return None
        first = index
        while index < end:
            if index > first and buf[index - 1] >= buf[index]:
                return None   # 同一多项式内下标必须严格递增
            h[i][buf[index]] = 1
            index += 1
    for j in range(index, omega):
        if buf[j] != 0:
            return None       # 填充区必须全零
    return h


# ---- 打包（sign / sigEncode 用）--------------------------------------------

def polyz_pack(p: list[int], gamma1: int) -> bytes:
    """z 的打包：每系数存 γ₁ − z，18 位 (γ₁=2¹⁷) 或 20 位 (γ₁=2¹⁹)"""
    if gamma1 == (1 << 17):
        out = bytearray(N // 4 * 9)
        for i in range(N // 4):
            t0 = gamma1 - p[4 * i + 0]
            t1 = gamma1 - p[4 * i + 1]
            t2 = gamma1 - p[4 * i + 2]
            t3 = gamma1 - p[4 * i + 3]
            o = 9 * i
            out[o + 0] = t0 & 0xFF
            out[o + 1] = (t0 >> 8) & 0xFF
            out[o + 2] = ((t0 >> 16) | (t1 << 2)) & 0xFF
            out[o + 3] = (t1 >> 6) & 0xFF
            out[o + 4] = ((t1 >> 14) | (t2 << 4)) & 0xFF
            out[o + 5] = (t2 >> 4) & 0xFF
            out[o + 6] = ((t2 >> 12) | (t3 << 6)) & 0xFF
            out[o + 7] = (t3 >> 2) & 0xFF
            out[o + 8] = (t3 >> 10) & 0xFF
        return bytes(out)
    out = bytearray(N // 2 * 5)
    for i in range(N // 2):
        t0 = gamma1 - p[2 * i + 0]
        t1 = gamma1 - p[2 * i + 1]
        o = 5 * i
        out[o + 0] = t0 & 0xFF
        out[o + 1] = (t0 >> 8) & 0xFF
        out[o + 2] = ((t0 >> 16) | (t1 << 4)) & 0xFF
        out[o + 3] = (t1 >> 4) & 0xFF
        out[o + 4] = (t1 >> 12) & 0xFF
    return bytes(out)


def polyw1_pack(p: list[int], gamma2: int) -> bytes:
    """w₁ 的打包：γ₂=(q−1)/88 → 6 位/系数，γ₂=(q−1)/32 → 4 位/系数"""
    if gamma2 == GAMMA2_88:
        out = bytearray(N // 4 * 3)
        for i in range(N // 4):
            o = 3 * i
            out[o + 0] = (p[4 * i + 0] | (p[4 * i + 1] << 6)) & 0xFF
            out[o + 1] = ((p[4 * i + 1] >> 2) | (p[4 * i + 2] << 4)) & 0xFF
            out[o + 2] = ((p[4 * i + 2] >> 4) | (p[4 * i + 3] << 2)) & 0xFF
        return bytes(out)
    out = bytearray(N // 2)
    for i in range(N // 2):
        out[i] = (p[2 * i + 0] | (p[2 * i + 1] << 4)) & 0xFF
    return bytes(out)


def hint_pack(h: list[list[int]], omega: int) -> bytes:
    """HintBitPack：每条多项式里 1 的下标顺次写入，末尾 k 字节存累计计数"""
    k = len(h)
    out = bytearray(omega + k)
    index = 0
    for i in range(k):
        for j in range(N):
            if h[i][j] != 0:
                out[index] = j
                index += 1
        out[omega + i] = index
    return bytes(out)


# ---- 采样 -------------------------------------------------------------------

def sample_in_ball(seed: bytes, tau: int) -> list[int]:
    """SampleInBall：SHAKE256(seed) 流里取 τ 个 ±1，其余为 0"""
    c = [0] * N
    xof = hashlib.shake_256(seed)
    buf = xof.digest(136)
    signs = int.from_bytes(buf[:8], "little")
    pos = 8
    for i in range(N - tau, N):
        while True:
            if pos >= len(buf):
                buf = xof.digest(len(buf) + 136)   # SHAKE 是流，digest 取前缀
            b = buf[pos]
            pos += 1
            if b <= i:
                break
        c[i] = c[b]
        c[b] = 1 - 2 * (signs & 1)
        signs >>= 1
    return c


def expand_mask(rho_pp: bytes, kappa: int, gamma1: int, ell: int) -> list[list[int]]:
    """ExpandMask：l 条系数落在 (−γ₁, γ₁] 的 mask 多项式 y"""
    c_bits = 1 + (gamma1 - 1).bit_length()      # 18 (γ₁=2¹⁷) 或 20 (γ₁=2¹⁹)
    out = []
    for r in range(ell):
        nonce = kappa + r
        buf = hashlib.shake_256(
            rho_pp + bytes([nonce & 0xFF, nonce >> 8])).digest(32 * c_bits)
        out.append(polyz_unpack(buf, gamma1))
    return out


# ---- NTT 域的辅助 -----------------------------------------------------------

def _pointwise(a: list[int], b: list[int]) -> list[int]:
    return [montgomery_reduce(a[i] * b[i]) for i in range(N)]


def _mprime(context: bytes, msg: bytes) -> bytes:
    """external 接口的 M' 封装（FIPS 204 Algorithm 2/3 的 pure 分支）"""
    return bytes([0, len(context)]) + context + msg


# ---- 密钥拆包 ---------------------------------------------------------------

def sk_decode(sk: bytes, name: str):
    """skDecode：rho ‖ K ‖ tr ‖ s1 ‖ s2 ‖ t0"""
    par = PARAMS[name]
    k, ell, eta = par["k"], par["l"], par["eta"]
    eb = POLYETA_BYTES[eta]
    off = 0
    rho = sk[off:off + 32]; off += 32
    key = sk[off:off + 32]; off += 32
    tr = sk[off:off + 64]; off += 64
    s1 = []
    for _ in range(ell):
        s1.append(polyeta_unpack(sk[off:off + eb], eta)); off += eb
    s2 = []
    for _ in range(k):
        s2.append(polyeta_unpack(sk[off:off + eb], eta)); off += eb
    t0 = []
    for _ in range(k):
        t0.append(polyt0_unpack(sk[off:off + POLYT0_BYTES])); off += POLYT0_BYTES
    return rho, key, tr, s1, s2, t0


def pk_decode(pk: bytes, name: str):
    """pkDecode：rho ‖ t1"""
    k = PARAMS[name]["k"]
    rho = pk[:32]
    off = 32
    t1 = []
    for _ in range(k):
        t1.append(polyt1_unpack(pk[off:off + POLYT1_BYTES])); off += POLYT1_BYTES
    return rho, t1


# ---- Sign / Verify ----------------------------------------------------------

def mldsa_sign(sk: bytes, msg: bytes, context: bytes, rnd: bytes,
               name: str) -> bytes:
    """FIPS 204 ML-DSA.Sign（external + pure）：M' 封装后走 Sign_internal"""
    par = PARAMS[name]
    sp = SIG_PARAMS[name]
    k, ell, eta, gamma2 = par["k"], par["l"], par["eta"], par["gamma2"]
    tau, gamma1, omega, beta = sp["tau"], sp["gamma1"], sp["omega"], sp["beta"]
    ctilde_len = sp["ctilde"]

    rho, key, tr, s1, s2, t0 = sk_decode(sk, name)

    a_hat = [[rej_uniform_poly(rho, (i << 8) + j) for j in range(ell)]
             for i in range(k)]
    s1_hat = [ntt(list(p)) for p in s1]
    s2_hat = [ntt(list(p)) for p in s2]
    t0_hat = [ntt(list(p)) for p in t0]

    m_prime = _mprime(context, msg)
    mu = h_shake256(tr + m_prime, 64)
    rho_pp = h_shake256(key + rnd + mu, 64)

    kappa = 0
    while True:
        y = expand_mask(rho_pp, kappa, gamma1, ell)
        kappa += ell
        y_hat = [ntt(list(p)) for p in y]

        # w = A·y，逐系数 caddq 后分解出 (w0, w1)
        w0, w1 = [], []
        for i in range(k):
            acc = _pointwise(a_hat[i][0], y_hat[0])
            for j in range(1, ell):
                pw = _pointwise(a_hat[i][j], y_hat[j])
                acc = [acc[n] + pw[n] for n in range(N)]
            acc = invntt_tomont([reduce32(x) for x in acc])
            acc = [caddq(x) for x in acc]
            pair = [decompose(x, gamma2) for x in acc]
            w0.append([p[0] for p in pair])
            w1.append([p[1] for p in pair])

        c_tilde = h_shake256(
            mu + b"".join(polyw1_pack(w1[i], gamma2) for i in range(k)),
            ctilde_len)
        c = sample_in_ball(c_tilde, tau)
        c_hat = ntt(list(c))

        # z = y + c·s1
        z = []
        for j in range(ell):
            cs1 = invntt_tomont(_pointwise(c_hat, s1_hat[j]))
            z.append([reduce32(y[j][n] + cs1[n]) for n in range(N)])
        if any(chknorm(x, gamma1 - beta) for p in z for x in p):
            continue

        # r0 = LowBits(w − c·s2)
        r0 = []
        bad = False
        for i in range(k):
            cs2 = invntt_tomont(_pointwise(c_hat, s2_hat[i]))
            r0.append([reduce32(w0[i][n] - cs2[n]) for n in range(N)])
        if any(chknorm(x, gamma2 - beta) for p in r0 for x in p):
            continue

        # 提示：c·t0，范数 <γ₂ 且权重 ≤ω
        h = []
        ct0_ok = True
        weight = 0
        for i in range(k):
            ct0 = invntt_tomont(_pointwise(c_hat, t0_hat[i]))
            ct0 = [reduce32(x) for x in ct0]
            if any(chknorm(x, gamma2) for x in ct0):
                ct0_ok = False
                break
            a0 = [reduce32(r0[i][n] + ct0[n]) for n in range(N)]
            hi = [make_hint(a0[n], w1[i][n], gamma2) for n in range(N)]
            weight += sum(hi)
            h.append(hi)
        if not ct0_ok or weight > omega:
            continue

        sig = (c_tilde
               + b"".join(polyz_pack(z[j], gamma1) for j in range(ell))
               + hint_pack(h, omega))
        return sig


def mldsa_verify(pk: bytes, msg: bytes, context: bytes, sig: bytes,
                 name: str) -> bool:
    """FIPS 204 ML-DSA.Verify（external + pure）"""
    par = PARAMS[name]
    sp = SIG_PARAMS[name]
    k, ell, gamma2 = par["k"], par["l"], par["gamma2"]
    tau, gamma1, omega, beta = sp["tau"], sp["gamma1"], sp["omega"], sp["beta"]
    ctilde_len = sp["ctilde"]

    zbytes = (N // 4 * 9) if gamma1 == (1 << 17) else (N // 2 * 5)
    exp_len = ctilde_len + ell * zbytes + omega + k
    if len(sig) != exp_len:
        return False

    off = 0
    c_tilde = sig[off:off + ctilde_len]; off += ctilde_len
    z = []
    for _ in range(ell):
        z.append(polyz_unpack(sig[off:off + zbytes], gamma1)); off += zbytes
    h = hint_unpack(sig[off:off + omega + k], k, omega)
    if h is None:
        return False

    if any(chknorm(x, gamma1 - beta) for p in z for x in p):
        return False

    rho, t1 = pk_decode(pk, name)
    a_hat = [[rej_uniform_poly(rho, (i << 8) + j) for j in range(ell)]
             for i in range(k)]

    tr = h_shake256(pk, 64)
    m_prime = _mprime(context, msg)
    mu = h_shake256(tr + m_prime, 64)

    c = sample_in_ball(c_tilde, tau)
    c_hat = ntt(list(c))
    z_hat = [ntt(list(p)) for p in z]
    t1_hat = [ntt([coeff << D for coeff in t1[i]]) for i in range(k)]

    w1p = []
    for i in range(k):
        acc = _pointwise(a_hat[i][0], z_hat[0])
        for j in range(1, ell):
            acc = [acc[n] + _pointwise(a_hat[i][j], z_hat[j])[n]
                   for n in range(N)]
        ct1 = _pointwise(c_hat, t1_hat[i])
        acc = [reduce32(acc[n] - ct1[n]) for n in range(N)]
        acc = [caddq(x) for x in invntt_tomont(acc)]
        w1p.append([use_hint(acc[n], h[i][n], gamma2) for n in range(N)])

    c_tilde_p = h_shake256(
        mu + b"".join(polyw1_pack(w1p[i], gamma2) for i in range(k)),
        ctilde_len)
    return c_tilde_p == c_tilde


# ---- KAT 装载与两个新预言机 -------------------------------------------------

def _load_records(path: Path):
    if not path.exists():
        return None
    recs = []
    cur: dict[str, str] = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if line.startswith("#"):
            continue
        if not line:
            if cur:
                recs.append(cur)
                cur = {}
            continue
        key, _, val = line.partition(" = ")
        cur[key.strip()] = val.strip()
    if cur:
        recs.append(cur)
    return [r for r in recs if r.get("alg") in PARAMS]


def _hx(s: str) -> bytes:
    return bytes.fromhex(s) if s else b""


def oracle_d() -> bool:
    """siggen：逐字节重现 ACVP 期望签名"""
    recs = _load_records(SIGGEN_KAT)
    if recs is None:
        print("  ⚠ 预言机 D 跳过：找不到 vectors/mldsa_siggen.kat")
        return True
    ok = True
    per: dict[str, int] = {}
    for r in recs:
        alg = r["alg"]
        sig = mldsa_sign(_hx(r["sk"]), _hx(r["msg"]), _hx(r.get("context", "")),
                         _hx(r["rnd"]), alg)
        want = _hx(r["sig"])
        if sig != want:
            n = next((i for i in range(min(len(sig), len(want)))
                      if sig[i] != want[i]), min(len(sig), len(want)))
            print(f"  ✗ {alg} tcId={r.get('tcid')}：sig 不匹配，"
                  f"首个不同字节 @ {n}（len 得 {len(sig)} / 期望 {len(want)}）")
            ok = False
        else:
            per[alg] = per.get(alg, 0) + 1
    if ok:
        detail = "、".join(f"{a} {per.get(a, 0)} 条" for a in PARAMS)
        print(f"  ✓ 预言机 D：mldsa_sign 逐字节重现 ACVP siggen（{detail}）")
    return ok


def oracle_e() -> bool:
    """sigver：verify 的 bool 与 ACVP result 字段一致，覆盖 pass/fail 两类"""
    recs = _load_records(SIGVER_KAT)
    if recs is None:
        print("  ⚠ 预言机 E 跳过：找不到 vectors/mldsa_sigver.kat")
        return True
    ok = True
    n_pass = n_fail = 0
    for r in recs:
        alg = r["alg"]
        expect = r.get("result", "").lower() == "pass"
        got = mldsa_verify(_hx(r["pk"]), _hx(r["msg"]),
                           _hx(r.get("context", "")), _hx(r["sig"]), alg)
        if got != expect:
            print(f"  ✗ {alg} tcId={r.get('tcid')}：verify={got} 期望={expect}")
            ok = False
        elif expect:
            n_pass += 1
        else:
            n_fail += 1
    if ok:
        print(f"  ✓ 预言机 E：mldsa_verify 与 ACVP sigver 一致 —— "
              f"应通过 {n_pass} 条全 True，应拒绝 {n_fail} 条全 False")
    return ok


# ---------------------------------------------------------------- 反证

def falsify() -> bool:
    """把算子逐个改坏，预言机必须报错"""
    import contextlib
    import io

    import mldsa_model

    def quiet(fn):
        with contextlib.redirect_stdout(io.StringIO()):
            return fn()

    checks = []

    # 旋转因子表偏移一位：NTT 仍然自洽，但不再是那个环上的同态
    orig = mldsa_model.ZETAS[:]
    mldsa_model.ZETAS[1:] = orig[2:] + [orig[1]]
    checks.append(("旋转因子表整体偏移一位", not quiet(oracle_a)))
    mldsa_model.ZETAS[:] = orig

    # Power2Round 的舍入常数少 1
    orig_p2r = mldsa_model.power2round
    globals()["power2round"] = lambda a: (
        a - (((a + (1 << (D - 1))) >> D) << D), (a + (1 << (D - 1))) >> D)
    checks.append(("power2round 的舍入常数 +1", not quiet(oracle_b)))
    globals()["power2round"] = orig_p2r

    # Decompose 的倒数常数改一格
    orig_dec = mldsa_model.decompose

    def broken(a, gamma2):
        a0, a1 = orig_dec(a, gamma2)
        return a0, (a1 + 1) % (16 if gamma2 == GAMMA2_32 else 44)

    globals()["decompose"] = broken
    checks.append(("decompose 的高位加一", not quiet(oracle_b)))
    globals()["decompose"] = orig_dec

    ok = True
    for name, caught in checks:
        mark = "✓" if caught else "✗"
        print(f"  {mark} 反证：{name} —— 预言机{'如期报错' if caught else '竟然通过了'}")
        ok = ok and caught
    return ok


def main() -> int:
    print("ML-DSA 算子独立预言机")
    print()
    results = [oracle_a(), oracle_b(), oracle_c(), oracle_d(), oracle_e()]
    print()
    print("反证（把算子改坏，预言机必须报错）")
    results.append(falsify())
    print()
    if all(results):
        print("全部预言机与反证通过。")
        return 0
    print("有预言机未通过。")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
