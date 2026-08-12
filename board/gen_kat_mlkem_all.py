#!/usr/bin/env python3
"""从 NIST ACVP 向量生成三套 ML-KEM 参数集的板上 KAT 头文件

    python3 board/gen_kat_mlkem_all.py > board/kat_mlkem_all.h

为什么另起一个头文件、而不是把现有的 board/kat_vectors.h 扩开：
那一份里 ML-KEM-512 的三组向量连同 AES/SM4/SM3 的向量已经在真硅上跑通过
（24/24），是**已验证的既有资产**。往里面塞新东西要动它的结构，等于把一份
验过的东西重新变成没验过的。新的一份表驱动、三套参数集一起覆盖，
旧的原样留着 —— 两份各自独立，出了问题也能立刻分辨是谁的。

向量来源：vectors/acvp/ML-KEM-{keyGen,encapDecap}-FIPS203/
（NIST ACVP-Server 的官方向量，prompt.json + expectedResults.json 配对）
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
V = os.path.join(ROOT, 'vectors', 'acvp')

# pset 编号与 RTL 的 param_set 一致：0=512 1=768 2=1024
SETS = [('ML-KEM-512', 0), ('ML-KEM-768', 1), ('ML-KEM-1024', 2)]
NKG, NEN, NDE = 2, 2, 2      # 每套参数集取几组，够证明就行，别把头文件撑爆


def load(sub):
    p = json.load(open(os.path.join(V, sub, 'prompt.json')))
    r = json.load(open(os.path.join(V, sub, 'expectedResults.json')))
    res = {}
    for g in r['testGroups']:
        for t in g['tests']:
            res[t['tcId']] = t
    return p, res


def emit(name, hexstr):
    b = bytes.fromhex(hexstr)
    out = ["static const unsigned char %s[%d] = {" % (name, len(b))]
    for i in range(0, len(b), 12):
        out.append("    " + ", ".join("0x%02x" % x for x in b[i:i + 12]) + ",")
    out[-1] = out[-1].rstrip(',')
    out.append("};\n")
    return "\n".join(out)


def main():
    w = sys.stdout.write
    w("// 自动生成，请勿手改 —— board/gen_kat_mlkem_all.py\n")
    w("// 来源：NIST ACVP-Server 官方向量 ML-KEM-{keyGen,encapDecap}-FIPS203\n")
    w("// 覆盖 ML-KEM-512 / 768 / 1024 三套参数集，每套 KeyGen/Encaps/Decaps 各 %d 组\n" % NKG)
    w("#ifndef KAT_MLKEM_ALL_H\n#define KAT_MLKEM_ALL_H\n\n")

    kgp, kgr = load('ML-KEM-keyGen-FIPS203')
    edp, edr = load('ML-KEM-encapDecap-FIPS203')

    kg_rows, en_rows, de_rows = [], [], []

    for setname, pset in SETS:
        tag = setname.replace('ML-KEM-', 'k')
        # ---- KeyGen ----
        grp = next(g for g in kgp['testGroups'] if g['parameterSet'] == setname)
        for i, t in enumerate(grp['tests'][:NKG]):
            r = kgr[t['tcId']]
            w(emit("%s_kg%d_d" % (tag, i), t['d']))
            w(emit("%s_kg%d_z" % (tag, i), t['z']))
            w(emit("%s_kg%d_ek" % (tag, i), r['ek']))
            w(emit("%s_kg%d_dk" % (tag, i), r['dk']))
            kg_rows.append('    { %d, %d, %s_kg%d_d, %s_kg%d_z, %s_kg%d_ek,'
                           ' sizeof %s_kg%d_ek, %s_kg%d_dk, sizeof %s_kg%d_dk },'
                           % (pset, t['tcId'], tag, i, tag, i, tag, i,
                              tag, i, tag, i, tag, i))
        # ---- Encaps ----
        grp = next(g for g in edp['testGroups']
                   if g['parameterSet'] == setname and g['function'] == 'encapsulation')
        for i, t in enumerate(grp['tests'][:NEN]):
            r = edr[t['tcId']]
            w(emit("%s_en%d_ek" % (tag, i), t['ek']))
            w(emit("%s_en%d_m" % (tag, i), t['m']))
            w(emit("%s_en%d_c" % (tag, i), r['c']))
            w(emit("%s_en%d_k" % (tag, i), r['k']))
            en_rows.append('    { %d, %d, %s_en%d_ek, sizeof %s_en%d_ek,'
                           ' %s_en%d_m, %s_en%d_c, sizeof %s_en%d_c, %s_en%d_k },'
                           % (pset, t['tcId'], tag, i, tag, i, tag, i,
                              tag, i, tag, i, tag, i))
        # ---- Decaps ----
        grp = next(g for g in edp['testGroups']
                   if g['parameterSet'] == setname and g['function'] == 'decapsulation')
        # decapsulation 组的 dk 在 group 层，c 在 test 层（ACVP 的结构如此）
        gdk = grp.get('dk')
        for i, t in enumerate(grp['tests'][:NDE]):
            r = edr[t['tcId']]
            dk = t.get('dk') or gdk
            w(emit("%s_de%d_dk" % (tag, i), dk))
            w(emit("%s_de%d_c" % (tag, i), t['c']))
            w(emit("%s_de%d_k" % (tag, i), r['k']))
            de_rows.append('    { %d, %d, %s_de%d_dk, sizeof %s_de%d_dk,'
                           ' %s_de%d_c, sizeof %s_de%d_c, %s_de%d_k },'
                           % (pset, t['tcId'], tag, i, tag, i, tag, i,
                              tag, i, tag, i))

    w("""
/* pset 与 RTL 的 param_set 编号一致：0=512 1=768 2=1024 */
typedef struct {
    int                  pset, tc;
    const unsigned char *d, *z, *ek;
    unsigned             ek_len;
    const unsigned char *dk;
    unsigned             dk_len;
} mlkem_kg_vec;

typedef struct {
    int                  pset, tc;
    const unsigned char *ek;
    unsigned             ek_len;
    const unsigned char *m, *c;
    unsigned             c_len;
    const unsigned char *k;
} mlkem_en_vec;

typedef struct {
    int                  pset, tc;
    const unsigned char *dk;
    unsigned             dk_len;
    const unsigned char *c;
    unsigned             c_len;
    const unsigned char *k;
} mlkem_de_vec;

""")
    w("static const mlkem_kg_vec MLKEM_KG[] = {\n%s\n};\n\n" % "\n".join(kg_rows))
    w("static const mlkem_en_vec MLKEM_EN[] = {\n%s\n};\n\n" % "\n".join(en_rows))
    w("static const mlkem_de_vec MLKEM_DE[] = {\n%s\n};\n\n" % "\n".join(de_rows))
    w("static const char *const MLKEM_SET_NAME[3] = "
      "{ \"ML-KEM-512\", \"ML-KEM-768\", \"ML-KEM-1024\" };\n\n")
    w("#endif /* KAT_MLKEM_ALL_H */\n")


if __name__ == '__main__':
    main()
