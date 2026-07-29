#!/usr/bin/env python3
"""ML-KEM 数据通路算子的**独立预言机**

`vectors/rtl/*.hex` 由 `export_vectors.py` 从 `ref_model` 生成，所以 cocotb 里
"RTL == 向量 == 模型"三方一致实际只等价于 `RTL == ref_model`。一个自洽但错误的
压缩常数、一个反了顺序的位并行掩码，照样能让三方一致成立。

本文件给 ref_model 里的压缩、采样、基乘、字节编码各配一道**不同来源**的校验：

预言机 A  压缩/解压：按 FIPS 203 §4.2.1 的定义用有理数算 round(2^d/q·x)，
          与 ref_model 的整数写法在**整个输入域**上逐值比对（d ∈ {1,4,5,10,11}，
          输入只有 q = 3329 种取值，所以是穷举而不是抽样）。

预言机 B  CBD：按 FIPS 203 Alg 8 的定义逐比特数汉明重量，与 ref_model 的位并行
          写法比对。每个系数占用的比特组穷举，其余比特取零/取随机，
          再叠加大量全宽随机样本 —— 覆盖"组内正确"与"组间不串扰"两件事。

预言机 C  基乘：在 Z_q[x]/(x²−ζ) 上按定义展开多项式乘法，不走 Montgomery 域，
          最后比较是否相差一个 2⁻¹⁶ 因子。

预言机 D  用这些算子重建 ML-KEM KeyGen，比对 NIST ACVP 向量。
          矩阵 Â 的拒绝采样走 ref_model.rej_pair，s/e 的采样走 ref_model.cbd2/cbd3，
          t̂ 的累加走 ref_model.basemul，最后的 ek/dk 打包走 ref_model.encode12。
          能逐字节重现 ⇒ 这些算子就是真实 ML-KEM 用的那几个，
          而不只是"某组自洽的公式"。

用法：python3 hardware/model/mlkem_oracle.py
"""
from __future__ import annotations

import hashlib
import math
import random
import sys
from fractions import Fraction
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ref_model import (  # noqa: E402
    Q, ZETAS, barrett_reduce, basemul, cbd2, cbd3, compress, decode12,
    decompress, encode12, montgomery_reduce, ntt, rej_pair,
)

KAT = Path(__file__).resolve().parents[2] / "vectors" / "mlkem_keygen.kat"

# 参数集：k, eta1
PARAMS = {"ML-KEM-512": (2, 3), "ML-KEM-768": (3, 2), "ML-KEM-1024": (4, 2)}

D_VALUES = (1, 4, 5, 10, 11)


# ---------------------------------------------------------------- 预言机 A

def compress_definition(x: int, d: int) -> int:
    """FIPS 203 §4.2.1 的定义式，用有理数算，round 取半值向上"""
    v = Fraction((1 << d) * (x % Q), Q)
    return math.floor(v + Fraction(1, 2)) % (1 << d)


def decompress_definition(y: int, d: int) -> int:
    v = Fraction(Q * y, 1 << d)
    return math.floor(v + Fraction(1, 2))


def oracle_a() -> bool:
    ok = True
    for d in D_VALUES:
        for x in range(Q):
            if compress(x, d) != compress_definition(x, d):
                print(f"  ✗ compress d={d} x={x}: "
                      f"{compress(x, d)} != {compress_definition(x, d)}")
                ok = False
                break
        for y in range(1 << d):
            if decompress(y, d) != decompress_definition(y, d):
                print(f"  ✗ decompress d={d} y={y}")
                ok = False
                break
        # FIPS 203 对压缩误差的界：|Decompress(Compress(x)) − x mod± q| ≤ round(q/2^(d+1))
        bound = math.floor(Fraction(Q, 1 << (d + 1)) + Fraction(1, 2))
        for x in range(Q):
            e = (decompress(compress(x, d), d) - x) % Q
            if e > Q // 2:
                e -= Q
            if abs(e) > bound:
                print(f"  ✗ d={d} x={x}: 压缩误差 {e} 超出界 {bound}")
                ok = False
                break
    # 12 位编解码：往返必须还原折回 [0, q) 后的系数
    rng = random.Random(20260731)
    for _ in range(20000):
        c0 = rng.randrange(-Q, Q)
        c1 = rng.randrange(-Q, Q)
        if decode12(*encode12(c0, c1)) != (c0 % Q, c1 % Q):
            print(f"  ✗ encode12/decode12 往返不符：{(c0, c1)}")
            ok = False
            break
    if ok:
        print(f"  ✓ 预言机 A：d ∈ {list(D_VALUES)}，对全部 {Q} 个输入逐值验证 "
              f"compress/decompress 与 FIPS 203 定义式一致")
        print("      并验证压缩误差不超过 round(q/2^(d+1)) 的理论界、"
              "encode12/decode12 往返还原")
    return ok


# ---------------------------------------------------------------- 预言机 B

def cbd_definition(bits: int, eta: int, n: int) -> list[int]:
    """FIPS 203 Alg 8 的定义：前 η 个比特的汉明重量减后 η 个的汉明重量"""
    out = []
    for i in range(n):
        x = sum((bits >> (2 * i * eta + k)) & 1 for k in range(eta))
        y = sum((bits >> (2 * i * eta + eta + k)) & 1 for k in range(eta))
        out.append(x - y)
    return out


def oracle_b() -> bool:
    rng = random.Random(20260729)
    ok = True

    # 组内穷举：每个系数占用的 2η 位取遍全部组合，其余位取零与随机
    for eta, fn, groups, width in ((2, cbd2, 8, 32), (3, cbd3, 4, 24)):
        for g in range(groups):
            for pattern in range(1 << (2 * eta)):
                for filler in (0, rng.getrandbits(width)):
                    v = filler & ~(((1 << (2 * eta)) - 1) << (2 * eta * g))
                    v |= pattern << (2 * eta * g)
                    got = fn(v)
                    want = cbd_definition(v, eta, groups)
                    if got != want:
                        print(f"  ✗ cbd{eta} 组 {g} 模式 {pattern:#x}: {got} != {want}")
                        ok = False
    # 全宽随机：验证组与组之间不串扰
    for eta, fn, groups, width in ((2, cbd2, 8, 32), (3, cbd3, 4, 24)):
        for _ in range(20000):
            v = rng.getrandbits(width)
            if fn(v) != cbd_definition(v, eta, groups):
                print(f"  ✗ cbd{eta} 全宽随机不一致：{v:#x}")
                ok = False
                break
    if ok:
        print("  ✓ 预言机 B：位并行的 cbd2/cbd3 与 FIPS 203 Alg 8 的逐比特定义一致")
        print("      （每个系数的比特组穷举 + 各 20000 条全宽随机）")
    return ok


# ---------------------------------------------------------------- 预言机 C

def basemul_definition(a0: int, a1: int, b0: int, b1: int, zeta: int) -> tuple[int, int]:
    """在 Z_q[x]/(x² − ζ) 上直接展开，不走 Montgomery 域

    ζ 端口上给的是 Montgomery 域的 ζ·2¹⁶，所以定义式里要先把它换回普通表示。
    """
    z = zeta * pow(2, -16, Q) % Q
    r0 = (a0 * b0 + a1 * b1 * z) % Q
    r1 = (a0 * b1 + a1 * b0) % Q
    return r0, r1


def oracle_c() -> bool:
    rng = random.Random(20260730)
    inv_mont = pow(2, -16, Q)
    ok = True
    for _ in range(20000):
        a0 = rng.randrange(-Q, Q)
        a1 = rng.randrange(-Q, Q)
        b0 = rng.randrange(-Q, Q)
        b1 = rng.randrange(-Q, Q)
        z = ZETAS[rng.randrange(len(ZETAS))]
        r0, r1 = basemul(a0, a1, b0, b1, z)
        # Montgomery 域的乘法比普通乘法多一个 2⁻¹⁶ 因子
        w0, w1 = basemul_definition(a0, a1, b0, b1, z)
        if (r0 - w0 * inv_mont) % Q != 0 or (r1 - w1 * inv_mont) % Q != 0:
            print(f"  ✗ basemul 与定义式差的不是 2⁻¹⁶ 因子：{(a0, a1, b0, b1, z)}")
            ok = False
            break
    if ok:
        print("  ✓ 预言机 C：basemul 与 Z_q[x]/(x²−ζ) 上按定义展开的乘法"
              "相差恰好一个 2⁻¹⁶ 因子（20000 组随机）")
    return ok


# ---------------------------------------------------------------- 预言机 D

def G(data: bytes) -> tuple[bytes, bytes]:
    h = hashlib.sha3_512(data).digest()
    return h[:32], h[32:]


def H(data: bytes) -> bytes:
    return hashlib.sha3_256(data).digest()


def sample_ntt(rho: bytes, i: int, j: int) -> list[int]:
    """SampleNTT（FIPS 203 Alg 7），取候选一步走 ref_model.rej_pair"""
    out: list[int] = []
    nblocks = 1
    while True:
        buf = hashlib.shake_128(rho + bytes([i, j])).digest(168 * nblocks)
        out = []
        pos = 0
        while pos + 3 <= len(buf) and len(out) < 256:
            d1, d2 = rej_pair(buf[pos], buf[pos + 1], buf[pos + 2])
            pos += 3
            if d1 < Q:
                out.append(d1)
            if d2 < Q and len(out) < 256:
                out.append(d2)
        if len(out) == 256:
            return out
        nblocks += 1


def sample_poly_cbd(sigma: bytes, nonce: int, eta: int) -> list[int]:
    """PRF + CBD，逐块走 ref_model.cbd2 / cbd3"""
    buf = hashlib.shake_256(sigma + bytes([nonce])).digest(64 * eta)
    out: list[int] = []
    if eta == 2:
        for off in range(0, len(buf), 4):
            out.extend(cbd2(int.from_bytes(buf[off:off + 4], "little")))
    else:
        for off in range(0, len(buf), 3):
            out.extend(cbd3(int.from_bytes(buf[off:off + 3], "little")))
    return out


def poly_basemul(a: list[int], b: list[int]) -> list[int]:
    """逐点乘：128 对系数，相邻两对用 ±ζ，每一对走 ref_model.basemul"""
    r = [0] * 256
    for i in range(64):
        z = ZETAS[64 + i]
        for start, zz in ((4 * i, z), (4 * i + 2, -z)):
            r[start], r[start + 1] = basemul(
                a[start], a[start + 1], b[start], b[start + 1], zz)
    return r


def poly_tobytes(p: list[int]) -> bytes:
    """ByteEncode12，逐对系数走 ref_model.encode12"""
    out = bytearray(384)
    for i in range(128):
        out[3 * i], out[3 * i + 1], out[3 * i + 2] = encode12(p[2 * i], p[2 * i + 1])
    return bytes(out)


def mlkem_keygen(d: bytes, z: bytes, name: str) -> tuple[bytes, bytes]:
    """FIPS 203 ML-KEM.KeyGen_internal(d, z)，全部算子取自 ref_model"""
    k, eta1 = PARAMS[name]
    rho, sigma = G(d + bytes([k]))

    a_hat = [[sample_ntt(rho, j, i) for j in range(k)] for i in range(k)]

    s = [sample_poly_cbd(sigma, n, eta1) for n in range(k)]
    e = [sample_poly_cbd(sigma, k + n, eta1) for n in range(k)]

    s_hat = [ntt(list(x)) for x in s]
    e_hat = [ntt(list(x)) for x in e]

    f = (1 << 32) % Q          # 1353，把普通表示搬进 Montgomery 域
    t_hat = []
    for i in range(k):
        acc = poly_basemul(a_hat[i][0], s_hat[0])
        for j in range(1, k):
            prod = poly_basemul(a_hat[i][j], s_hat[j])
            acc = [acc[n] + prod[n] for n in range(256)]
        acc = [barrett_reduce(x) for x in acc]
        acc = [montgomery_reduce(x * f) for x in acc]
        acc = [acc[n] + e_hat[i][n] for n in range(256)]
        t_hat.append([barrett_reduce(x) for x in acc])

    ek = b"".join(poly_tobytes(p) for p in t_hat) + rho
    dk = b"".join(poly_tobytes(p) for p in s_hat) + ek + H(ek) + z
    return ek, dk


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


def oracle_d() -> bool:
    recs = load_kat()
    if recs is None:
        print("  ⚠ 预言机 D 跳过：找不到 vectors/mlkem_keygen.kat"
              "（先跑 tools/fetch_vectors.sh）")
        return True
    ok = True
    n = 0
    for r in recs:
        ek, dk = mlkem_keygen(bytes.fromhex(r["d"]), bytes.fromhex(r["z"]), r["alg"])
        if ek != bytes.fromhex(r["ek"]):
            print(f"  ✗ {r['alg']} tcId={r.get('tcid')}：ek 不匹配")
            ok = False
        elif dk != bytes.fromhex(r["dk"]):
            print(f"  ✗ {r['alg']} tcId={r.get('tcid')}：dk 不匹配（ek 对了）")
            ok = False
        n += 1
    if ok:
        print(f"  ✓ 预言机 D：用 rej_pair / cbd2 / cbd3 / basemul / encode12 重建 "
              f"ML-KEM KeyGen，{n} 条 NIST ACVP 向量的 ek/dk 逐字节重现")
        print("      ⇒ 这些算子是真实 ML-KEM 用的那几个，不只是某组自洽的公式")
    return ok


# ---------------------------------------------------------------- 反证

def falsify() -> bool:
    """反证：把算子逐个改坏，预言机必须报错。

    只断言"改对了会通过"不足以说明测试有效 —— 还要确认"改错了会失败"。
    """
    import ref_model

    checks = []

    # 压缩常数从 q/2 改成 q/2+1（四舍五入的分界点挪一格）
    orig_compress = ref_model.compress
    ref_model.compress = lambda x, d: ((((x % Q) << d) + Q // 2 + 1) // Q) & ((1 << d) - 1)
    globals()["compress"] = ref_model.compress
    checks.append(("compress 的舍入常数 +1", not oracle_a_quiet()))
    ref_model.compress = orig_compress
    globals()["compress"] = orig_compress

    # CBD 的两半互换（符号翻转）
    orig_cbd2 = ref_model.cbd2
    ref_model.cbd2 = lambda r: [-v for v in orig_cbd2(r)]
    globals()["cbd2"] = ref_model.cbd2
    checks.append(("cbd2 符号翻转", not oracle_b_quiet()))
    ref_model.cbd2 = orig_cbd2
    globals()["cbd2"] = orig_cbd2

    # 基乘里 ζ 用错一格
    orig_basemul = ref_model.basemul
    ref_model.basemul = lambda a0, a1, b0, b1, z: orig_basemul(a0, a1, b0, b1, z + 1)
    globals()["basemul"] = ref_model.basemul
    checks.append(("basemul 的 ζ 偏移一格", not oracle_c_quiet()))
    ref_model.basemul = orig_basemul
    globals()["basemul"] = orig_basemul

    ok = True
    for name, caught in checks:
        mark = "✓" if caught else "✗"
        print(f"  {mark} 反证：{name} —— 预言机{'如期报错' if caught else '竟然通过了'}")
        ok = ok and caught
    return ok


def _quiet(fn):
    import contextlib
    import io
    with contextlib.redirect_stdout(io.StringIO()):
        return fn()


def oracle_a_quiet() -> bool:
    return _quiet(oracle_a)


def oracle_b_quiet() -> bool:
    return _quiet(oracle_b)


def oracle_c_quiet() -> bool:
    return _quiet(oracle_c)


def main() -> int:
    print("ML-KEM 算子独立预言机")
    print()
    results = [oracle_a(), oracle_b(), oracle_c(), oracle_d()]
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
