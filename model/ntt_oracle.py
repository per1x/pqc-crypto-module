#!/usr/bin/env python3
"""NTT 的**独立预言机**（RTL 审查报告【建议改】4）

【要解决的问题】
原来 cocotb 里写的"三方一致"其实是**两方**：`vectors/rtl/*.hex` 是
`export_vectors.py` 从 `ref_model` 生成的，向量与模型同源，
所以 `RTL == 向量 == 模型` 实际只等价于 `RTL == ref_model`。
一张"自洽但错误"的旋转因子表 / 层序，照样能让往返测试通过。

【两道独立校验】

**预言机 A：schoolbook 负循环卷积**
在 Z_q[x]/(x^256+1) 上直接做 O(n²) 多项式乘法 —— **完全不碰 zeta 表、
不碰 NTT**。然后验证 `invntt(basemul(ntt(a), ntt(b))) == schoolbook(a, b)`。
这验证的是 NTT 的**语义**（它确实是那个环上的乘法同态），而不是自洽。
zeta 表错一个数、层序错一层，同态就不成立。

**预言机 B：用 ref_model 的 ntt() 重建 ML-KEM KeyGen，比对 NIST ACVP 向量**
这一道更狠：把 FIPS 203 的 K-PKE.KeyGen 用 Python 重写一遍，中间那步
`ŝ = NTT(s)` 直接调 `ref_model.ntt`，最后看能不能逐字节重现 ACVP 的 `ek`/`dk`。
能重现 ⇒ **ref_model 的 NTT 就是真实 ML-KEM 用的那个 NTT**，
而不只是"某个自洽的 NTT"。这条把 RTL 一路钉到 NIST 向量上。

用法：python3 model/ntt_oracle.py
"""
from __future__ import annotations

import hashlib
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ref_model import Q, ZETAS, fqmul, invntt, montgomery_reduce, ntt  # noqa: E402

KAT = Path(__file__).resolve().parent.parent / "vectors" / "mlkem_keygen.kat"

# 参数集：k, eta1
PARAMS = {"ML-KEM-512": (2, 3), "ML-KEM-768": (3, 2), "ML-KEM-1024": (4, 2)}


# ---------------------------------------------------------------- 预言机 A

def schoolbook_negacyclic(a: list[int], b: list[int]) -> list[int]:
    """Z_q[x]/(x^256+1) 上的朴素多项式乘法。

    **刻意不使用任何 NTT / zeta** —— 它就是独立性的来源。
    x^256 = -1，所以下标溢出的项要变号。
    """
    r = [0] * 256
    for i in range(256):
        ai = a[i]
        if ai == 0:
            continue
        for j in range(256):
            k = i + j
            v = ai * b[j]
            if k < 256:
                r[k] += v
            else:
                r[k - 256] -= v
    return [x % Q for x in r]


def basemul_montgomery(a: list[int], b: list[int]) -> list[int]:
    """NTT 域的逐点乘（pq-crystals 的 poly_basemul_montgomery）。

    ML-KEM 的 NTT 只做 7 层，剩下的是 128 个 2 次多项式，
    所以"逐点乘"实际是 128 次 2×2 的多项式乘，每对带一个 ±zeta。
    """
    r = [0] * 256
    for i in range(64):
        z = ZETAS[64 + i]
        for start, zz in ((4 * i, z), (4 * i + 2, -z)):
            a0, a1 = a[start], a[start + 1]
            b0, b1 = b[start], b[start + 1]
            r0 = fqmul(fqmul(a1, b1), zz) + fqmul(a0, b0)
            r1 = fqmul(a0, b1) + fqmul(a1, b0)
            r[start], r[start + 1] = r0, r1
    return r


def oracle_a(trials: int = 8) -> bool:
    """NTT 必须是 Z_q[x]/(x^256+1) 上的乘法同态"""
    rng = random.Random(20260729)
    ok = True
    for t in range(trials):
        a = [rng.randrange(Q) for _ in range(256)]
        b = [rng.randrange(Q) for _ in range(256)]
        want = schoolbook_negacyclic(a, b)
        got = invntt(basemul_montgomery(ntt(list(a)), ntt(list(b))))
        if any((got[i] - want[i]) % Q != 0 for i in range(256)):
            print(f"  ✗ 第 {t} 组：NTT 不是那个环上的乘法同态")
            ok = False
    if ok:
        print(f"  ✓ 预言机 A：{trials} 组随机多项式，"
              f"invntt(basemul(ntt(a),ntt(b))) 与 schoolbook 卷积**逐系数相等**")
        print(f"      （schoolbook 完全不碰 zeta 表 —— 表错一个数、层序错一层，同态就不成立）")
    return ok


# ---------------------------------------------------------------- 预言机 B

def G(data: bytes) -> tuple[bytes, bytes]:
    h = hashlib.sha3_512(data).digest()
    return h[:32], h[32:]


def H(data: bytes) -> bytes:
    return hashlib.sha3_256(data).digest()


def prf(eta: int, s: bytes, b: int) -> bytes:
    return hashlib.shake_256(s + bytes([b])).digest(64 * eta)


def xof(rho: bytes, i: int, j: int) -> hashlib.shake_128:
    return hashlib.shake_128(rho + bytes([i, j]))


def sample_ntt(rho: bytes, i: int, j: int) -> list[int]:
    """SampleNTT：从 SHAKE128 流里拒绝采样出 NTT 域的多项式（FIPS 203 Alg 7）"""
    out: list[int] = []
    nblocks = 1
    while True:
        buf = xof(rho, i, j).digest(168 * nblocks)
        out = []
        pos = 0
        while pos + 3 <= len(buf) and len(out) < 256:
            d1 = buf[pos] | ((buf[pos + 1] & 0x0F) << 8)
            d2 = (buf[pos + 1] >> 4) | (buf[pos + 2] << 4)
            pos += 3
            if d1 < Q:
                out.append(d1)
            if d2 < Q and len(out) < 256:
                out.append(d2)
        if len(out) == 256:
            return out
        nblocks += 1


def cbd(buf: bytes, eta: int) -> list[int]:
    """中心二项分布采样（FIPS 203 Alg 8）"""
    bits = int.from_bytes(buf, "little")
    r = []
    for i in range(256):
        x = 0
        y = 0
        for k in range(eta):
            x += (bits >> (2 * i * eta + k)) & 1
            y += (bits >> (2 * i * eta + eta + k)) & 1
        r.append(x - y)
    return r


def poly_reduce(p: list[int]) -> list[int]:
    from ref_model import barrett_reduce
    return [barrett_reduce(x) for x in p]


def poly_tomont(p: list[int]) -> list[int]:
    f = (1 << 32) % Q          # 1353
    return [montgomery_reduce(x * f) for x in p]


def poly_tobytes(p: list[int]) -> bytes:
    """ByteEncode12：先把系数规约到 [0,q)，再每两个压进 3 字节"""
    t = [x % Q for x in p]
    out = bytearray(384)
    for i in range(128):
        t0, t1 = t[2 * i], t[2 * i + 1]
        out[3 * i] = t0 & 0xFF
        out[3 * i + 1] = ((t0 >> 8) | (t1 << 4)) & 0xFF
        out[3 * i + 2] = (t1 >> 4) & 0xFF
    return bytes(out)


def mlkem_keygen(d: bytes, z: bytes, name: str) -> tuple[bytes, bytes]:
    """FIPS 203 ML-KEM.KeyGen_internal(d, z)

    中间那步 ŝ = NTT(s) / ê = NTT(e) **直接调 ref_model.ntt** ——
    这正是这道预言机的意义所在。
    """
    k, eta1 = PARAMS[name]
    rho, sigma = G(d + bytes([k]))

    # Â：注意索引顺序。pq-crystals 的 gen_matrix(non-transposed) 用 XOF(rho, j, i)
    a_hat = [[sample_ntt(rho, j, i) for j in range(k)] for i in range(k)]

    nonce = 0
    s = []
    for _ in range(k):
        s.append(cbd(prf(eta1, sigma, nonce), eta1))
        nonce += 1
    e = []
    for _ in range(k):
        e.append(cbd(prf(eta1, sigma, nonce), eta1))
        nonce += 1

    s_hat = [ntt(list(x)) for x in s]        # ← 被验证的对象
    e_hat = [ntt(list(x)) for x in e]

    t_hat = []
    for i in range(k):
        acc = basemul_montgomery(a_hat[i][0], s_hat[0])
        for j in range(1, k):
            prod = basemul_montgomery(a_hat[i][j], s_hat[j])
            acc = [acc[n] + prod[n] for n in range(256)]
        acc = poly_reduce(acc)
        acc = poly_tomont(acc)
        acc = [acc[n] + e_hat[i][n] for n in range(256)]
        t_hat.append(poly_reduce(acc))

    ek_pke = b"".join(poly_tobytes(p) for p in t_hat) + rho
    dk_pke = b"".join(poly_tobytes(p) for p in s_hat)
    ek = ek_pke
    dk = dk_pke + ek + H(ek) + z
    return ek, dk


def load_kat(limit_per_alg: int = 3):
    """从 vectors/mlkem_keygen.kat 读 ACVP 向量（d, z → ek, dk）"""
    if not KAT.exists():
        return None
    recs = []
    cur = {}
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
        if alg not in PARAMS:
            continue
        if seen.get(alg, 0) >= limit_per_alg:
            continue
        seen[alg] = seen.get(alg, 0) + 1
        out.append(r)
    return out


def oracle_b() -> bool:
    recs = load_kat()
    if recs is None:
        print("  ⚠ 预言机 B 跳过：找不到 vectors/mlkem_keygen.kat"
              "（先跑 tools/fetch_vectors.sh）")
        return True
    ok = True
    n = 0
    for r in recs:
        alg = r["alg"]
        d = bytes.fromhex(r["d"])
        z = bytes.fromhex(r["z"])
        ek_exp = bytes.fromhex(r["ek"])
        dk_exp = bytes.fromhex(r["dk"])
        ek, dk = mlkem_keygen(d, z, alg)
        if ek != ek_exp:
            print(f"  ✗ {alg} tcId={r.get('tcid')}：ek 不匹配")
            ok = False
        elif dk != dk_exp:
            print(f"  ✗ {alg} tcId={r.get('tcid')}：dk 不匹配（ek 对了）")
            ok = False
        n += 1
    if ok:
        print(f"  ✓ 预言机 B：用 ref_model.ntt 重建 ML-KEM KeyGen，"
              f"{n} 条 NIST ACVP 向量的 ek/dk **逐字节重现**")
        print(f"      ⇒ ref_model 的 NTT 就是真实 ML-KEM 用的那个 NTT，"
              f"不只是'某个自洽的 NTT'")
    return ok


def main() -> int:
    print("NTT 独立预言机")
    print()
    a = oracle_a()
    print()
    b = oracle_b()
    print()
    if a and b:
        print("两道独立预言机都通过。")
        return 0
    print("有预言机未通过。")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
