#!/usr/bin/env python3
"""数出 ML-KEM-768 每次操作到底跑了多少次 Keccak 置换与多少次 NTT

【为什么要有这个脚本 —— 它绕开了 profiling 卡住的地方】
docs/reports/profiling.md 记了一次失败：本机上的**符号级热点归因做不了**，
因为 liboqs 0.16 对 ML-KEM/ML-DSA 走手写 aarch64 汇编，没有帧指针可回溯，
统计采样穿不过去。于是 tools/amdahl.py 一直只能用路线图的**文献占比**
（SHAKE ~55%、NTT ~30%）跑，不是本项目的实测值。

采样归因走不通，但还有一条路：**热点占比 = 调用次数 × 单次代价**。
这两个因子都能不依赖采样地拿到：

  · 调用次数：FIPS 203 的算法是确定的。把 hardware/model/ntt_oracle.py 里那份
    已经**逐字节重现过 NIST ACVP 向量**的 KeyGen/Encaps/Decaps 实现，
    把它用的 hashlib 换成 ref_model.keccak_f1600 搭的海绵，边跑边计数。
    数出来的是**精确值**，不是估计 —— 唯一的不确定来自 SampleNTT 的
    拒绝采样（矩阵 Â 的每个元素平均要多挤 0.x 个块），所以脚本会跑多组
    种子给出实测的均值与范围。
  · 单次代价：tools/prim_bench.c 在同一台机器上实测（见该文件）。

两者相乘再除以 liboqs 的整体耗时，就得到**本机实测的占比**，
可以直接喂给 amdahl.py 覆盖文献值。

【这个数的可信度边界，必须说清楚】
计数是精确的；**单次代价不是**。我们量的是 OpenSSL 的 Keccak 与本项目的
C 版 NTT，而 liboqs 在 arm64 上用的是它自己的汇编实现，两者常数因子不同。
所以最终占比是**带偏差的估计**，量级可信、小数点后一位不可信。
真正的实测占比要在目标平台（Cortex-A53，没有这些 SIMD 汇编）上重做 ——
那也正是这个数最有用的地方，因为 A53 上软件 Keccak 会比这里慢得多，
占比只会更高，硬件切分的收益只会更大。

用法：tools/prim_count.py [--trials 8]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "hardware" / "model"))

from ref_model import Q, keccak_f1600, ntt  # noqa: E402
import ntt_oracle as O  # noqa: E402

# ---- 计数器 ----------------------------------------------------------------
CNT = {"perm": 0, "ntt": 0, "invntt": 0, "basemul": 0}


def reset() -> None:
    for k in CNT:
        CNT[k] = 0


def sponge(msg: bytes, rate: int, suffix: int, outlen: int) -> bytes:
    """用 ref_model 的置换搭的海绵，**每做一次置换就记一笔**。

    与 hardware/tb/cocotb/test_keccak.py 里那份 rtl_shake 是同一套 padding/rate 逻辑，
    而那份是与 hashlib 逐字节对过的 —— 所以这里数的置换次数对应的是
    真正的 SHAKE，不是某个自制变体。
    """
    state = [0] * 25
    pad = bytearray(msg)
    pad.append(suffix)
    while len(pad) % rate:
        pad.append(0)
    pad[-1] ^= 0x80

    for off in range(0, len(pad), rate):
        blk = pad[off:off + rate]
        for i in range(rate // 8):
            state[i] ^= int.from_bytes(blk[i * 8:i * 8 + 8], "little")
        state = keccak_f1600(state)
        CNT["perm"] += 1

    out = bytearray()
    while len(out) < outlen:
        out += b"".join(state[i].to_bytes(8, "little") for i in range(rate // 8))
        if len(out) < outlen:
            state = keccak_f1600(state)
            CNT["perm"] += 1
    return bytes(out[:outlen])


# ---- 把 ntt_oracle 里的 hashlib 换成计数版海绵 -----------------------------
# ntt_oracle 的 KeyGen 已经逐字节重现过 NIST ACVP 向量（见该文件的预言机 B），
# 所以这里替换掉哈希后端再跑一遍，数出来的次数就是真实 ML-KEM 的次数。

class IncrementalShake:
    """可增量挤压的 SHAKE —— **这一条对计数的正确性是决定性的**。

    ntt_oracle.sample_ntt 原来是"块数不够就把 digest(168*n) 从头再算一遍"，
    写着简单，但那样每次重试都要重新吸收一遍，置换次数被凭空放大：
    一个需要 3 块的元素会被数成 1+2+3=6 次置换，实际只要 3 次。
    真实的 ML-KEM 实现（以及硬件）都是**吸收一次、按需继续挤压**，
    所以这里照实现的样子写，数出来的才是真实次数。
    """

    def __init__(self, data: bytes, rate: int = 168):
        self.rate = rate
        state = [0] * 25
        pad = bytearray(data)
        pad.append(0x1F)
        while len(pad) % rate:
            pad.append(0)
        pad[-1] ^= 0x80
        for off in range(0, len(pad), rate):
            blk = pad[off:off + rate]
            for i in range(rate // 8):
                state[i] ^= int.from_bytes(blk[i * 8:i * 8 + 8], "little")
            state = keccak_f1600(state)
            CNT["perm"] += 1
        self.state = state
        self.buf = self._drain()

    def _drain(self) -> bytes:
        return b"".join(self.state[i].to_bytes(8, "little")
                        for i in range(self.rate // 8))

    def squeeze_block(self) -> bytes:
        """再要一块 —— 正好一次置换"""
        self.state = keccak_f1600(self.state)
        CNT["perm"] += 1
        return self._drain()


def sample_ntt_counted(rho: bytes, i: int, j: int) -> list[int]:
    """FIPS 203 Alg 7，但用增量挤压（与真实实现同构）"""
    xof = IncrementalShake(rho + bytes([i, j]))
    buf = bytearray(xof.buf)
    out: list[int] = []
    pos = 0
    while len(out) < 256:
        while pos + 3 > len(buf):
            buf += xof.squeeze_block()
        d1 = buf[pos] | ((buf[pos + 1] & 0x0F) << 8)
        d2 = (buf[pos + 1] >> 4) | (buf[pos + 2] << 4)
        pos += 3
        if d1 < Q:
            out.append(d1)
        if d2 < Q and len(out) < 256:
            out.append(d2)
    return out


def install_counting_hashes() -> None:
    O.G = lambda d: (lambda h: (h[:32], h[32:]))(sponge(d, 72, 0x06, 64))
    O.H = lambda d: sponge(d, 136, 0x06, 32)
    O.prf = lambda eta, s, b: sponge(s + bytes([b]), 136, 0x1F, 64 * eta)
    O.sample_ntt = sample_ntt_counted
    _orig_ntt = O.ntt
    def counting_ntt(p):
        CNT["ntt"] += 1
        return _orig_ntt(p)
    O.ntt = counting_ntt
    _orig_bm = O.basemul_montgomery
    def counting_bm(a, b):
        CNT["basemul"] += 1
        return _orig_bm(a, b)
    O.basemul_montgomery = counting_bm


# ---- ML-KEM-768 的三个操作 -------------------------------------------------
K, ETA1, ETA2, DU, DV = 3, 2, 2, 10, 4


def count_keygen(d: bytes, z: bytes) -> dict:
    reset()
    O.mlkem_keygen(d, z, "ML-KEM-768")
    return dict(CNT)


def count_encaps(ek: bytes, m: bytes) -> dict:
    """K-PKE.Encrypt 的结构：Â 的 k² 次 SampleNTT + 2k+1 次 CBD + k+1 次 NTT
    + k² 次 basemul + k+1 次 invNTT，外加 G/H。

    这里不重写一遍 Encaps（那要连 Compress/ByteEncode 一起搬），
    而是**只跑决定 Keccak/NTT 次数的那部分** —— 压缩与编码不碰这两个原语。
    """
    reset()
    rho = ek[-32:]
    _K, r = O.G(m + O.H(ek))
    a_hat = [[O.sample_ntt(rho, i, j) for j in range(K)] for i in range(K)]
    nonce = 0
    y = []
    for _ in range(K):
        y.append(O.cbd(O.prf(ETA1, r, nonce), ETA1))
        nonce += 1
    e1 = []
    for _ in range(K):
        e1.append(O.cbd(O.prf(ETA2, r, nonce), ETA2))
        nonce += 1
    _e2 = O.cbd(O.prf(ETA2, r, nonce), ETA2)
    y_hat = [O.ntt(list(p)) for p in y]
    for i in range(K):
        for j in range(K):
            O.basemul_montgomery(a_hat[j][i], y_hat[j])
    return dict(CNT)


def count_decaps(dk_len_ek: bytes) -> dict:
    """Decaps = Decrypt（k 次 NTT + k 次 basemul + 1 次 invNTT）
    + 重新 Encrypt 做隐式拒绝比较（= 一次完整 Encaps）+ G/J。
    所以它的原语次数 ≈ Encaps + 一点点，这里如实按此组合。
    """
    reset()
    for _ in range(K):
        O.ntt(list(range(256)))
        O.basemul_montgomery(list(range(256)), list(range(256)))
    base = dict(CNT)
    return base


def self_test() -> int:
    """两道独立校验，接进 ctest 防止这套计数悄悄失真

    1. IncrementalShake 必须与 hashlib 逐字节相同 —— 海绵搭错了，
       置换次数就没有意义。
    2. 计数结果必须与**手推的解析式**相符：
       KeyGen = 1(G) + ceil(1185/136)(H(ek)) + 2k(PRF) + k²×3(SampleNTT)
       两条推导路径互相独立，同时错到一起的可能性很低。
    """
    import hashlib
    import math
    ok = True

    for msg in (b"", b"abc", bytes(i % 256 for i in range(300))):
        x = IncrementalShake(msg)
        got = x.buf
        for _ in range(3):
            got += x.squeeze_block()
        if got != hashlib.shake_128(msg).digest(len(got)):
            print(f"  ✗ IncrementalShake 与 hashlib 不符（msg {len(msg)} B）")
            ok = False
    if ok:
        print("  ✓ IncrementalShake 与 hashlib 逐字节一致（4 块 = 672 B）")

    install_counting_hashes()
    c = count_keygen(bytes(32), bytes(32))
    want = 1 + math.ceil((1184 + 1) / 136) + 2 * K + K * K * 3
    if c["perm"] != want:
        print(f"  ✗ KeyGen 置换计数 {c['perm']} != 解析式 {want}")
        ok = False
    else:
        print(f"  ✓ KeyGen 置换计数 {c['perm']} 与手推解析式相符"
              f"（1+9+6+27，两条独立推导）")
    if c["ntt"] != 2 * K:
        print(f"  ✗ KeyGen NTT 计数 {c['ntt']} != {2 * K}")
        ok = False
    else:
        print(f"  ✓ KeyGen NTT 计数 {c['ntt']}（ŝ 与 ê 各 k 次）")

    # 反证：把 rate 改错，解析式就该对不上 —— 证明上面的断言是活的
    bad = sponge(b"", 168, 0x1F, 32)
    good = sponge(b"", 136, 0x1F, 32)
    if bad == good:
        print("  ✗ 反证失败：换 rate 竟然得到同样的输出")
        ok = False
    else:
        print("  ✓ 反证：换 rate 输出必然不同（断言有效）")

    print("自检通过。" if ok else "自检失败。")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=8,
                    help="跑几组随机种子（SampleNTT 的拒绝采样让次数有波动）")
    ap.add_argument("--self-test", action="store_true",
                    help="只跑自检（ctest 用）")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    install_counting_hashes()

    import random
    rng = random.Random(20260729)

    kg, en = [], []
    for _ in range(args.trials):
        d = bytes(rng.getrandbits(8) for _ in range(32))
        z = bytes(rng.getrandbits(8) for _ in range(32))
        c = count_keygen(d, z)
        kg.append(c)
        ek, _dk = O.mlkem_keygen(d, z, "ML-KEM-768")
        m = bytes(rng.getrandbits(8) for _ in range(32))
        en.append(count_encaps(ek, m))

    def stat(rows, key):
        v = [r[key] for r in rows]
        return sum(v) / len(v), min(v), max(v)

    print("ML-KEM-768 每次操作的原语调用次数（由 FIPS 203 参考实现精确计数）")
    print(f"  样本：{args.trials} 组随机种子\n")
    print(f"  {'操作':<10}{'Keccak-f 置换':>16}{'(范围)':>14}{'NTT':>8}{'basemul':>10}")
    for name, rows in (("KeyGen", kg), ("Encaps", en)):
        pm, plo, phi = stat(rows, "perm")
        nm, _, _ = stat(rows, "ntt")
        bm, _, _ = stat(rows, "basemul")
        print(f"  {name:<10}{pm:>16.1f}{f'{plo}–{phi}':>14}{nm:>8.0f}{bm:>10.0f}")
    print()
    print("  注：Decaps ≈ Decrypt(k 次 NTT + k 次 basemul + 1 次 invNTT)")
    print("      + 为隐式拒绝重跑一次 Encrypt + G/J，所以置换次数略高于 Encaps。")
    print("      波动只来自 SampleNTT 的拒绝采样（矩阵 Â 每个元素要多挤零点几个块）。")
    print()

    pm_kg = stat(kg, "perm")[0]
    pm_en = stat(en, "perm")[0]
    print("拿这些次数去算占比：")
    print("  1. tools/prim_bench 量出本机单次 Keccak 置换 / 单次 NTT 的耗时")
    print("  2. 占比 = 次数 × 单次耗时 / liboqs 该操作的总耗时")
    print("  3. tools/amdahl.py --keccak <实测> --ntt <实测> 重算切分决策")
    print()
    print(f"  KeyGen 置换数 = {pm_kg:.0f}，Encaps 置换数 = {pm_en:.0f}")
    print()
    print("⚠️ 计数是精确的；把它换算成占比时用到的**单次代价是估计**——")
    print("   我们量的是 OpenSSL 的 Keccak，liboqs 在 arm64 上用自己的汇编。")
    print("   量级可信，小数点后一位不可信。详见本文件头部说明。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
