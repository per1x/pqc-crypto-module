#!/usr/bin/env python3
"""ACVP 的 ML-DSA 向量 → 板上用的 C 头文件

    python3 tools/mldsa_kat_to_c.py > board/src/kat_mldsa.h

为什么要这一步：**板子上没有 python3，也不该在板上解析文件**。
上板程序 (board/src/mldsa_hwtest.c) 是静态链接、拷过去直接跑的，它唯一
认得的输入就是编译期嵌进去的字节数组。向量文件 vectors/*.kat 是 .gitignore
的（可由 tools/fetch_vectors.sh + tools/acvp_to_kat.py 重新生成），所以
**生成出来的头文件要提交**，与 board/src/kat_mlkem_all.h 同一个口径。

============================================================================
【挑哪些条目，为什么】
============================================================================
· 每个参数集各挑 2 条 —— 够证明"这条路是通的"，再多只是把头文件撑大，
  而单条向量对不上时的定位能力并不随条数增长。
· siggen **只挑 deterministic = 1 的条目**（rnd = 0³²）。hedged 条目每次
  取新随机数，签出来必然与向量里的 σ 不同 —— 拿它当判据等于自己给自己
  制造一堆假失败。RTL 侧的入口就是"rnd 永远在输入流里，确定性就写 32 个零"
  （见 hardware/rtl/bus/mldsa_axi.v 文件头）。
· sigver **pass 与 fail 各挑 2 条**。只验通过的那一半是不够的：一个
  恒返回"通过"的实现能把它全过掉。
· 挑**最短的**（msg + ctx 字节数）。一个字节要走一笔 AXI 写，短的省时间；
  而且最短的那几条正是 cocotb 用例里已经在仿真里绿过的那几条，所以上板
  失败时可以直接排除"这条向量本身有问题"。

============================================================================
【两条硬上限，超了必须在这里滤掉】
============================================================================
· msg ≤ 8192：sign.v / verify.v 里的 u_msg 是 AW=13。超了 RTL 会在 START
  处拒绝（LEN_ERR），那是正确行为，但拿它当"算法对不对"的用例是浪费。
  ACVP 里确实有超过 8192 的条目（实测 sigver 有 8195 字节的 msg）。
· ctx ≤ 255：FIPS 204 的 |ctx| ≤ 255，mldsa_axi 在 params_ok 里判。
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VEC = ROOT / "vectors"

SETS = [("ML-DSA-44", 0), ("ML-DSA-65", 1), ("ML-DSA-87", 2)]
TAG = {0: "d44", 1: "d65", 2: "d87"}

N_KG = 2            # 每参数集 KeyGen 条数
N_SG = 2            # 每参数集 Sign 条数（确定性）
N_SV = 2            # 每参数集 Verify 条数（pass / fail **各** 这么多）

MSGMAX = 8192       # 核里 u_msg 是 AW=13
CTXMAX = 255        # FIPS 204

# FIPS 204 表 2 —— 用来核对挑出来的向量长度对不对号。
# 长度对不上不是"少测一条"，是"KAT 文件和 RTL 说的不是同一个参数集"，
# 那种情况下继续生成只会把问题推到板上才现形。
PK = {0: 1312, 1: 1952, 2: 2592}
SK = {0: 2560, 1: 4032, 2: 4896}
SIG = {0: 2420, 1: 3309, 2: 4627}


def load(path: Path) -> list[dict]:
    """与 hardware/model/mldsa_oracle.py 的 _load_records 同一份解析

    格式：`名 = 值`，空行分记录，# 注释。
    """
    if not path.exists():
        sys.exit(f"找不到 {path} —— 先跑 tools/fetch_vectors.sh 与 "
                 f"tools/acvp_to_kat.py")
    recs, cur = [], {}
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
    return recs


def hx(r: dict, k: str) -> bytes:
    return bytes.fromhex(r.get(k, ""))


def fits(r: dict) -> bool:
    return len(hx(r, "msg")) <= MSGMAX and len(hx(r, "context")) <= CTXMAX


def short(r: dict) -> tuple:
    """按"要经 AXI 搬多少字节"排序；tcid 兜底，保证挑选是确定性的"""
    return (len(r.get("msg", "")) + len(r.get("context", "")),
            int(r.get("tcid", "0")))


def pick(recs: list[dict], alg: str, n: int, pred=None, by=None) -> list[dict]:
    out = [r for r in recs if r.get("alg") == alg and (pred is None or pred(r))]
    if by is not None:
        out = sorted(out, key=by)
    if len(out) < n:
        sys.exit(f"{alg}：符合条件的记录只有 {len(out)} 条，要 {n} 条")
    return out[:n]


class Emitter:
    def __init__(self):
        self.chunks: list[str] = []
        self.total = 0

    def blob(self, name: str, b: bytes) -> str:
        """吐一个字节数组，返回引用它的表达式

        空的（典型是 ctx_len = 0）不能写成 `unsigned char x[0]` —— 那不是
        合法的 ISO C。空的一律给 NULL，C 侧按 len == 0 处理。
        """
        if not b:
            return "NULL, 0"
        lines = [f"static const unsigned char {name}[{len(b)}] = {{"]
        for i in range(0, len(b), 12):
            lines.append("    " + ", ".join(f"0x{x:02x}" for x in b[i:i + 12])
                         + ",")
        lines[-1] = lines[-1].rstrip(",")
        lines.append("};")
        self.chunks.append("\n".join(lines))
        self.total += len(b)
        return f"{name}, {len(b)}"


def main() -> None:
    kg_recs = load(VEC / "mldsa_keygen.kat")
    sg_recs = load(VEC / "mldsa_siggen.kat")
    sv_recs = load(VEC / "mldsa_sigver.kat")

    em = Emitter()
    kg_rows, sg_rows, sv_rows = [], [], []

    for alg, pset in SETS:
        t = TAG[pset]

        # ---- KeyGen：ACVP 顺序里的前几条 ----
        for i, r in enumerate(pick(kg_recs, alg, N_KG)):
            xi, pk, sk = hx(r, "seed"), hx(r, "pk"), hx(r, "sk")
            assert len(xi) == 32, f"{alg} kg tcid={r['tcid']} 的 seed 不是 32 字节"
            assert len(pk) == PK[pset] and len(sk) == SK[pset], \
                f"{alg} kg tcid={r['tcid']} 的 pk/sk 长度与 FIPS 204 表 2 对不上"
            kg_rows.append("    { %d, %s, %s, %s, %s }," % (
                pset, r["tcid"],
                em.blob(f"{t}_kg{i}_xi", xi).split(",")[0],
                em.blob(f"{t}_kg{i}_pk", pk),
                em.blob(f"{t}_kg{i}_sk", sk)))

        # ---- Sign：只要确定性条目（rnd = 0³²）----
        for i, r in enumerate(pick(sg_recs, alg, N_SG,
                                   lambda x: x.get("deterministic") == "1"
                                   and fits(x), short)):
            sk, msg = hx(r, "sk"), hx(r, "msg")
            ctx, rnd, sig = hx(r, "context"), hx(r, "rnd"), hx(r, "sig")
            assert rnd == bytes(32), \
                f"{alg} sg tcid={r['tcid']} 标了 deterministic 但 rnd 不是全零"
            assert len(sk) == SK[pset] and len(sig) == SIG[pset], \
                f"{alg} sg tcid={r['tcid']} 的 sk/sig 长度对不上号"
            sg_rows.append("    { %d, %s, %s, %s, %s, %s }," % (
                pset, r["tcid"],
                em.blob(f"{t}_sg{i}_sk", sk),
                em.blob(f"{t}_sg{i}_ctx", ctx),
                em.blob(f"{t}_sg{i}_msg", msg),
                em.blob(f"{t}_sg{i}_sig", sig)))

        # ---- Verify：pass 与 fail 各 N_SV 条 ----
        for want, mark in (("pass", 1), ("fail", 0)):
            for i, r in enumerate(pick(sv_recs, alg, N_SV,
                                       lambda x, w=want: x.get("result") == w
                                       and fits(x), short)):
                pk, sig = hx(r, "pk"), hx(r, "sig")
                msg, ctx = hx(r, "msg"), hx(r, "context")
                assert len(pk) == PK[pset] and len(sig) == SIG[pset], \
                    f"{alg} sv tcid={r['tcid']} 的 pk/sig 长度对不上号"
                sv_rows.append("    { %d, %s, %s, %s, %s, %s, %d }," % (
                    pset, r["tcid"],
                    em.blob(f"{t}_sv{want}{i}_pk", pk),
                    em.blob(f"{t}_sv{want}{i}_sig", sig),
                    em.blob(f"{t}_sv{want}{i}_ctx", ctx),
                    em.blob(f"{t}_sv{want}{i}_msg", msg),
                    mark))

    w = sys.stdout.write
    w("// 自动生成，请勿手改 —— tools/mldsa_kat_to_c.py\n")
    w("// 来源：NIST ACVP-Server 官方向量 ML-DSA-{keyGen,sigGen,sigVer}-FIPS204\n")
    w("//      （经 tools/acvp_to_kat.py 摊平成 vectors/mldsa_*.kat）\n")
    w("// 覆盖 ML-DSA-44/65/87 三套参数集：\n")
    w("//   KeyGen 各 %d 条；Sign 各 %d 条（**确定性**，rnd = 0³²）；\n"
      % (N_KG, N_SG))
    w("//   Verify 各 %d 条 pass + %d 条 fail（只验通过的一半挡不住"
      "\"恒返回通过\"）\n" % (N_SV, N_SV))
    w("//\n")
    w("// ⚠️ 挑选时已滤掉 msg > %d（核里 u_msg 是 AW=13）与 ctx > %d"
      "（FIPS 204）的条目。\n" % (MSGMAX, CTXMAX))
    w("#ifndef KAT_MLDSA_H\n#define KAT_MLDSA_H\n\n")
    w("#include <stddef.h>   /* NULL —— 空 ctx 用它，`unsigned char x[0]`"
      " 不是合法 C */\n\n")
    w("\n".join(em.chunks))
    w("""

/* pset 与 RTL 的 MODE[3:2] 一致：0=44 1=65 2=87 */
typedef struct {
    int                  pset, tc;
    const unsigned char *xi;            /* 32 字节种子 ξ */
    const unsigned char *pk;
    unsigned             pk_len;
    const unsigned char *sk;
    unsigned             sk_len;
} mldsa_kg_vec;

/* 输入字节流：sk ‖ rnd(0³²) ‖ ctx ‖ msg —— rnd 不在表里，它恒为 32 个零 */
typedef struct {
    int                  pset, tc;
    const unsigned char *sk;
    unsigned             sk_len;
    const unsigned char *ctx;
    unsigned             ctx_len;
    const unsigned char *msg;
    unsigned             msg_len;
    const unsigned char *sig;
    unsigned             sig_len;
} mldsa_sg_vec;

/* 输入字节流：pk ‖ sig ‖ ctx ‖ msg；expect_ok 是 ACVP 的 result 字段 */
typedef struct {
    int                  pset, tc;
    const unsigned char *pk;
    unsigned             pk_len;
    const unsigned char *sig;
    unsigned             sig_len;
    const unsigned char *ctx;
    unsigned             ctx_len;
    const unsigned char *msg;
    unsigned             msg_len;
    int                  expect_ok;
} mldsa_sv_vec;

""")
    w("static const mldsa_kg_vec MLDSA_KG[] = {\n%s\n};\n\n" % "\n".join(kg_rows))
    w("static const mldsa_sg_vec MLDSA_SG[] = {\n%s\n};\n\n" % "\n".join(sg_rows))
    w("static const mldsa_sv_vec MLDSA_SV[] = {\n%s\n};\n\n" % "\n".join(sv_rows))
    w('static const char *const MLDSA_SET_NAME[3] = '
      '{ "ML-DSA-44", "ML-DSA-65", "ML-DSA-87" };\n\n')
    w("#endif /* KAT_MLDSA_H */\n")

    print("嵌入向量共 %d 字节（%d KeyGen / %d Sign / %d Verify 条）"
          % (em.total, len(kg_rows), len(sg_rows), len(sv_rows)),
          file=sys.stderr)


if __name__ == "__main__":
    main()
