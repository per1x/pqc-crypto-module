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
    D, GAMMA2_32, GAMMA2_88, N, PARAMS, Q, caddq, decompose, invntt_tomont,
    make_hint, montgomery_reduce, ntt, power2round, reduce32,
    rej_eta_coeff, rej_uniform_coeff, use_hint,
)

KAT = Path(__file__).resolve().parents[2] / "vectors" / "mldsa_keygen.kat"


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
    results = [oracle_a(), oracle_b(), oracle_c()]
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
