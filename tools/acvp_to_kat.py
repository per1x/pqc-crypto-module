#!/usr/bin/env python3
"""ACVP (FIPS 203/204 最终版) JSON → 扁平黄金向量 .kat

路线图 §5.1.3 / §10.1：整机 KAT 一律用最终版 ACVP 向量，不用 round-3 .rsp。

输出格式（C 侧 60 行即可解析，将来 cocotb / RTL testbench 共用同一批文件）：

    # 注释
    alg = ML-KEM-768
    op = keygen
    tcid = 1
    d = <hex>
    ...
    <空行分隔记录>

只导出**本项目 crypto 后端能够驱动**的组；不能驱动的组显式记入 SKIPPED 注释头，
避免"悄悄少测了一半"。
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ACVP = ROOT / "vectors" / "acvp"
OUT = ROOT / "vectors"

# liboqs 后端能力边界（见 README「技术栈选择理由」）：
#   - ML-DSA 只暴露 external + pure 接口（sign_with_ctx_str）
#   - 不暴露 Sign_internal / HashML-DSA(preHash) / externalMu
DSA_SUPPORTED = {"signatureInterface": "external", "preHash": "pure"}


def load(subdir: str, name: str):
    return json.loads((ACVP / subdir / name).read_text())


def expected_index(subdir: str) -> dict[int, dict]:
    """tcId → 期望结果"""
    exp = load(subdir, "expectedResults.json")
    return {t["tcId"]: t for g in exp["testGroups"] for t in g["tests"]}


class Writer:
    def __init__(self, path: Path, source: str):
        self.path = path
        self.records: list[list[tuple[str, str]]] = []
        self.skipped: list[str] = []
        self.source = source

    def add(self, fields: list[tuple[str, str]]):
        self.records.append(fields)

    def skip(self, why: str):
        if why not in self.skipped:
            self.skipped.append(why)

    def flush(self):
        lines = [
            f"# 自动生成，请勿手改 —— tools/acvp_to_kat.py",
            f"# 来源: NIST ACVP-Server {self.source}",
            f"# 记录数: {len(self.records)}",
        ]
        for s in self.skipped:
            lines.append(f"# SKIPPED: {s}")
        lines.append("")
        for rec in self.records:
            for k, v in rec:
                lines.append(f"{k} = {v}")
            lines.append("")
        self.path.write_text("\n".join(lines))
        print(f"  {self.path.relative_to(ROOT)}: {len(self.records)} 条"
              + (f"  ({len(self.skipped)} 类跳过)" if self.skipped else ""))


def mlkem_keygen():
    sub = "ML-KEM-keyGen-FIPS203"
    p, exp = load(sub, "prompt.json"), expected_index(sub)
    w = Writer(OUT / "mlkem_keygen.kat", sub)
    for g in p["testGroups"]:
        for t in g["tests"]:
            e = exp[t["tcId"]]
            w.add([("alg", g["parameterSet"]), ("op", "keygen"), ("tcid", str(t["tcId"])),
                   ("d", t["d"]), ("z", t["z"]), ("ek", e["ek"]), ("dk", e["dk"])])
    w.flush()


def mlkem_encapdecap():
    sub = "ML-KEM-encapDecap-FIPS203"
    p, exp = load(sub, "prompt.json"), expected_index(sub)
    enc = Writer(OUT / "mlkem_encaps.kat", sub)
    dec = Writer(OUT / "mlkem_decaps.kat", sub)
    chk = Writer(OUT / "mlkem_keycheck.kat", sub)
    for g in p["testGroups"]:
        fn, ps = g["function"], g["parameterSet"]
        for t in g["tests"]:
            e = exp[t["tcId"]]
            if fn == "encapsulation":
                enc.add([("alg", ps), ("op", "encaps"), ("tcid", str(t["tcId"])),
                         ("ek", t["ek"]), ("m", t["m"]), ("c", e["c"]), ("k", e["k"])])
            elif fn == "decapsulation":
                dec.add([("alg", ps), ("op", "decaps"), ("tcid", str(t["tcId"])),
                         ("dk", t["dk"]), ("c", t["c"]), ("k", e["k"])])
            elif fn in ("encapsulationKeyCheck", "decapsulationKeyCheck"):
                which = "ek" if fn.startswith("encap") else "dk"
                chk.add([("alg", ps), ("op", "keycheck"), ("tcid", str(t["tcId"])),
                         ("which", which), (which, t[which]),
                         ("result", "pass" if e["testPassed"] else "fail")])
    for w in (enc, dec, chk):
        w.flush()


def mldsa_keygen():
    sub = "ML-DSA-keyGen-FIPS204"
    p, exp = load(sub, "prompt.json"), expected_index(sub)
    w = Writer(OUT / "mldsa_keygen.kat", sub)
    for g in p["testGroups"]:
        for t in g["tests"]:
            e = exp[t["tcId"]]
            w.add([("alg", g["parameterSet"]), ("op", "keygen"), ("tcid", str(t["tcId"])),
                   ("seed", t["seed"]), ("pk", e["pk"]), ("sk", e["sk"])])
    w.flush()


def _dsa_group_ok(g, w) -> bool:
    for k, want in DSA_SUPPORTED.items():
        if g.get(k) != want:
            w.skip(f"{g['parameterSet']} {k}={g.get(k)} —— liboqs 后端未暴露该接口")
            return False
    return True


def mldsa_siggen():
    sub = "ML-DSA-sigGen-FIPS204"
    p, exp = load(sub, "prompt.json"), expected_index(sub)
    w = Writer(OUT / "mldsa_siggen.kat", sub)
    for g in p["testGroups"]:
        if not _dsa_group_ok(g, w):
            continue
        for t in g["tests"]:
            e = exp[t["tcId"]]
            # FIPS 204 确定性模式即 rnd = 0^32；非确定性模式 ACVP 显式给出 rnd。
            rnd = "00" * 32 if g["deterministic"] else t["rnd"]
            w.add([("alg", g["parameterSet"]), ("op", "siggen"), ("tcid", str(t["tcId"])),
                   ("deterministic", "1" if g["deterministic"] else "0"),
                   ("sk", t["sk"]), ("msg", t["message"]),
                   ("context", t.get("context", "")), ("rnd", rnd),
                   ("sig", e["signature"])])
    w.flush()


def mldsa_sigver():
    sub = "ML-DSA-sigVer-FIPS204"
    p, exp = load(sub, "prompt.json"), expected_index(sub)
    w = Writer(OUT / "mldsa_sigver.kat", sub)
    for g in p["testGroups"]:
        if not _dsa_group_ok(g, w):
            continue
        for t in g["tests"]:
            e = exp[t["tcId"]]
            w.add([("alg", g["parameterSet"]), ("op", "sigver"), ("tcid", str(t["tcId"])),
                   ("pk", t["pk"]), ("msg", t["message"]),
                   ("context", t.get("context", "")), ("sig", t["signature"]),
                   ("result", "pass" if e["testPassed"] else "fail")])
    w.flush()


def main() -> int:
    if not ACVP.exists():
        print(f"缺少 {ACVP}，先跑 tools/fetch_vectors.sh", file=sys.stderr)
        return 1
    OUT.mkdir(exist_ok=True)
    print("展平 ACVP 向量 →")
    mlkem_keygen()
    mlkem_encapdecap()
    mldsa_keygen()
    mldsa_siggen()
    mldsa_sigver()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
