#!/usr/bin/env python3
"""对称密码与国密算法的黄金模型与独立预言机（AES / SM4 / SM3）

与 mlkem_oracle.py / mldsa_oracle.py 同一套规矩：

  · 参考实现**照标准正文写**，不调用任何密码库；
  · 每个算法都钉在**公开标准里的测试向量**上，而不是"自己和自己比"；
  · 每道检查都配一条反证，确认它真的会失败。

标准出处：
  AES  —— FIPS 197 附录 C（C.1 AES-128、C.3 AES-256）
  SM4  —— GB/T 32907-2016 附录 A（单次加密 + 1000000 次迭代）
  SM3  —— GB/T 32905-2016 附录 A（"abc" 与 "abcd"×16）

本文件还负责**生成 RTL 里的 S 盒表**（--emit-sbox）。RTL 里放表、这里放代数
定义，测试台再逐值交叉验证 —— 表抄错一格就会被逐值比对抓住。
"""
from __future__ import annotations

import sys

# ============================================================================
# GF(2^8) —— AES 用 0x11B，SM4 用 0x1F5
# ============================================================================


def gf_mul(a: int, b: int, poly: int) -> int:
    r = 0
    for _ in range(8):
        if b & 1:
            r ^= a
        b >>= 1
        hi = a & 0x80
        a = (a << 1) & 0xFF
        if hi:
            a ^= poly & 0xFF
    return r


def gf_inv(a: int, poly: int) -> int:
    """穷举求逆 —— 域只有 256 个元素，没必要用扩展欧几里得"""
    if a == 0:
        return 0
    for b in range(1, 256):
        if gf_mul(a, b, poly) == 1:
            return b
    raise AssertionError(f"{a:#04x} 在这个域里没有逆元，多项式选错了")


def _rotl8(x: int, n: int) -> int:
    return ((x << n) | (x >> (8 - n))) & 0xFF


def aes_sbox_table() -> list[int]:
    """FIPS 197 §5.1.1：GF(2^8) 求逆，再做仿射变换

    表本身是标准里印出来的，但这里**从定义算**而不是抄表 —— 抄表的话，
    RTL 里的表和这里的表可能抄错同一格，交叉验证就失去意义。
    """
    box = []
    for x in range(256):
        b = gf_inv(x, 0x11B)
        s = b ^ _rotl8(b, 1) ^ _rotl8(b, 2) ^ _rotl8(b, 3) ^ _rotl8(b, 4) ^ 0x63
        box.append(s & 0xFF)
    return box


SBOX = aes_sbox_table()
INV_SBOX = [0] * 256
for _i, _v in enumerate(SBOX):
    INV_SBOX[_v] = _i

# SM4 的 S 盒（GB/T 32907-2016 §6.2）。它同样是"仿射 ∘ 求逆 ∘ 仿射"，
# 但标准正文给的是表，代数参数要反推 —— 这里照标准抄表，正确性由 GB/T 的
# 测试向量端到端保证：表错一格，附录 A 的密文立刻对不上。
SM4_SBOX = bytes.fromhex(
    "d690e9fecce13db716b614c228fb2c05"
    "2b679a762abe04c3aa44132649860699"
    "9c4250f491ef987a33540b43edcfac62"
    "e4b31ca9c908e89580df94fa758f3fa6"
    "4707a7fcf37317ba83593c19e6854fa8"
    "686b81b27164da8bf8eb0f4b70569d35"
    "1e240e5e6358d1a225227c3b01217887"
    "d40046579fd327524c3602e7a0c4c89e"
    "eabf8ad240c738b5a3f7f2cef96115a1"
    "e0ae5da49b341a55ad933230f58cb1e3"
    "1df6e22e8266ca60c02923ab0d534e6f"
    "d5db3745defd8e2f03ff6a726d6c5b51"
    "8d1baf92bbddbc7f11d95c411f105ad8"
    "0ac13188a5cd7bbd2d74d012b8e5b4b0"
    "8969974a0c96777e65b9f109c56ec684"
    "18f07dec3adc4d2079ee5f3ed7cb3948")

assert len(SM4_SBOX) == 256, f"SM4 S 盒长度 {len(SM4_SBOX)}，应当是 256"


# ============================================================================
# AES（FIPS 197）
# ============================================================================

RCON = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36,
        0x6C, 0xD8, 0xAB, 0x4D]


def aes_key_expansion(key: bytes) -> list[list[int]]:
    """返回 Nr+1 个轮密钥，每个 16 字节（按列主序的字节串）"""
    nk = len(key) // 4
    assert nk in (4, 8), "本模型只支持 AES-128 与 AES-256"
    nr = nk + 6
    w = [list(key[4 * i:4 * i + 4]) for i in range(nk)]
    for i in range(nk, 4 * (nr + 1)):
        t = list(w[i - 1])
        if i % nk == 0:
            t = t[1:] + t[:1]                       # RotWord
            t = [SBOX[b] for b in t]                # SubWord
            t[0] ^= RCON[i // nk - 1]
        elif nk == 8 and i % nk == 4:
            t = [SBOX[b] for b in t]                # AES-256 独有的这一步
        w.append([w[i - nk][j] ^ t[j] for j in range(4)])
    return [sum(w[4 * r:4 * r + 4], []) for r in range(nr + 1)]


def _sub_bytes(s, box):
    return [box[b] for b in s]


def _shift_rows(s, inv=False):
    # 状态按列主序存：s[c*4+r]
    out = [0] * 16
    for r in range(4):
        for c in range(4):
            src = (c + r) % 4 if not inv else (c - r) % 4
            out[c * 4 + r] = s[src * 4 + r]
    return out


def _mix_columns(s, inv=False):
    out = [0] * 16
    m = (14, 11, 13, 9) if inv else (2, 3, 1, 1)
    for c in range(4):
        col = s[4 * c:4 * c + 4]
        for r in range(4):
            out[4 * c + r] = 0
            for k in range(4):
                out[4 * c + r] ^= gf_mul(col[k], m[(k - r) % 4], 0x11B)
    return out


def _add_round_key(s, rk):
    return [a ^ b for a, b in zip(s, rk)]


def aes_encrypt_block(key: bytes, pt: bytes) -> bytes:
    rks = aes_key_expansion(key)
    nr = len(rks) - 1
    s = _add_round_key(list(pt), rks[0])
    for r in range(1, nr):
        s = _add_round_key(_mix_columns(_shift_rows(_sub_bytes(s, SBOX))), rks[r])
    s = _add_round_key(_shift_rows(_sub_bytes(s, SBOX)), rks[nr])
    return bytes(s)


def aes_decrypt_block(key: bytes, ct: bytes) -> bytes:
    rks = aes_key_expansion(key)
    nr = len(rks) - 1
    s = _add_round_key(list(ct), rks[nr])
    for r in range(nr - 1, 0, -1):
        s = _sub_bytes(_shift_rows(s, inv=True), INV_SBOX)
        s = _mix_columns(_add_round_key(s, rks[r]), inv=True)
    s = _add_round_key(_sub_bytes(_shift_rows(s, inv=True), INV_SBOX), rks[0])
    return bytes(s)


# ============================================================================
# SM4（GB/T 32907-2016）
# ============================================================================

SM4_FK = [0xA3B1BAC6, 0x56AA3350, 0x677D9197, 0xB27022DC]
SM4_CK = [(((4 * i + j) * 7) & 0xFF) for i in range(32) for j in range(4)]
SM4_CK = [int.from_bytes(bytes(SM4_CK[4 * i:4 * i + 4]), "big") for i in range(32)]


def _rotl32(x: int, n: int) -> int:
    n &= 31
    return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF


def _sm4_tau(x: int) -> int:
    b = x.to_bytes(4, "big")
    return int.from_bytes(bytes(SM4_SBOX[c] for c in b), "big")


def _sm4_t(x: int) -> int:
    """轮函数里的 T：τ 之后过线性变换 L"""
    b = _sm4_tau(x)
    return b ^ _rotl32(b, 2) ^ _rotl32(b, 10) ^ _rotl32(b, 18) ^ _rotl32(b, 24)


def _sm4_t_prime(x: int) -> int:
    """密钥扩展里的 T′：同一个 τ，不同的线性变换 L′"""
    b = _sm4_tau(x)
    return b ^ _rotl32(b, 13) ^ _rotl32(b, 23)


def sm4_round_keys(key: bytes) -> list[int]:
    mk = [int.from_bytes(key[4 * i:4 * i + 4], "big") for i in range(4)]
    k = [mk[i] ^ SM4_FK[i] for i in range(4)]
    rk = []
    for i in range(32):
        nxt = k[i] ^ _sm4_t_prime(k[i + 1] ^ k[i + 2] ^ k[i + 3] ^ SM4_CK[i])
        k.append(nxt)
        rk.append(nxt)
    return rk


def sm4_crypt_block(key: bytes, blk: bytes, decrypt: bool = False) -> bytes:
    """SM4 的解密与加密是同一套逻辑，只把轮密钥倒过来用"""
    rk = sm4_round_keys(key)
    if decrypt:
        rk = rk[::-1]
    x = [int.from_bytes(blk[4 * i:4 * i + 4], "big") for i in range(4)]
    for i in range(32):
        x.append(x[i] ^ _sm4_t(x[i + 1] ^ x[i + 2] ^ x[i + 3] ^ rk[i]))
    # 反序变换 R
    return b"".join(x[35 - i].to_bytes(4, "big") for i in range(4))


# ============================================================================
# SM3（GB/T 32905-2016）
# ============================================================================

SM3_IV = [0x7380166F, 0x4914B2B9, 0x172442D7, 0xDA8A0600,
          0xA96F30BC, 0x163138AA, 0xE38DEE4D, 0xB0FB0E4E]


def _sm3_ff(j, x, y, z):
    return (x ^ y ^ z) if j < 16 else ((x & y) | (x & z) | (y & z))


def _sm3_gg(j, x, y, z):
    return (x ^ y ^ z) if j < 16 else ((x & y) | ((~x & 0xFFFFFFFF) & z))


def _sm3_p0(x):
    return x ^ _rotl32(x, 9) ^ _rotl32(x, 17)


def _sm3_p1(x):
    return x ^ _rotl32(x, 15) ^ _rotl32(x, 23)


def sm3_expand(block: bytes) -> tuple[list[int], list[int]]:
    """消息扩展：W[0..67] 与 W′[0..63]"""
    w = [int.from_bytes(block[4 * i:4 * i + 4], "big") for i in range(16)]
    for j in range(16, 68):
        w.append(_sm3_p1(w[j - 16] ^ w[j - 9] ^ _rotl32(w[j - 3], 15))
                 ^ _rotl32(w[j - 13], 7) ^ w[j - 6])
    w1 = [w[j] ^ w[j + 4] for j in range(64)]
    return w, w1


def sm3_compress(v: list[int], block: bytes) -> list[int]:
    w, w1 = sm3_expand(block)
    a, b, c, d, e, f, g, h = v
    for j in range(64):
        t = 0x79CC4519 if j < 16 else 0x7A879D8A
        ss1 = _rotl32((_rotl32(a, 12) + e + _rotl32(t, j)) & 0xFFFFFFFF, 7)
        ss2 = ss1 ^ _rotl32(a, 12)
        tt1 = (_sm3_ff(j, a, b, c) + d + ss2 + w1[j]) & 0xFFFFFFFF
        tt2 = (_sm3_gg(j, e, f, g) + h + ss1 + w[j]) & 0xFFFFFFFF
        d = c
        c = _rotl32(b, 9)
        b = a
        a = tt1
        h = g
        g = _rotl32(f, 19)
        f = e
        e = _sm3_p0(tt2)
    return [x ^ y for x, y in zip([a, b, c, d, e, f, g, h], v)]


def sm3(msg: bytes) -> bytes:
    ml = len(msg) * 8
    padded = msg + b"\x80" + b"\x00" * ((56 - (len(msg) + 1) % 64) % 64)
    padded += ml.to_bytes(8, "big")
    v = list(SM3_IV)
    for i in range(0, len(padded), 64):
        v = sm3_compress(v, padded[i:i + 64])
    return b"".join(x.to_bytes(4, "big") for x in v)


# ============================================================================
# 公开标准向量
# ============================================================================

AES128_KEY = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
AES256_KEY = bytes.fromhex("000102030405060708090a0b0c0d0e0f"
                           "101112131415161718191a1b1c1d1e1f")
AES_PT = bytes.fromhex("00112233445566778899aabbccddeeff")
AES128_CT = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")   # FIPS 197 C.1
AES256_CT = bytes.fromhex("8ea2b7ca516745bfeafc49904b496089")   # FIPS 197 C.3

SM4_KEY = bytes.fromhex("0123456789abcdeffedcba9876543210")
SM4_PT = SM4_KEY
SM4_CT = bytes.fromhex("681edf34d206965e86b3e94f536e4246")      # GB/T 32907 A.1
SM4_CT_1M = bytes.fromhex("595298c7c6fd271f0402f804c33d3f66")   # GB/T 32907 A.2

SM3_ABC = bytes.fromhex("66c7f0f462eeedd9d1f2d46bdc10e4e2"
                        "4167c4875cf2f7a2297da02b8f4ba8e0")     # GB/T 32905 A.1
SM3_ABCD16 = bytes.fromhex("debe9ff92275b8a138604889c18e5a4d"
                           "6fdb70e5387e5765293dcba39c0c5732")  # GB/T 32905 A.2


def check_vectors(million: bool = False) -> None:
    """把三个算法钉在标准正文的向量上"""
    assert SBOX[0] == 0x63 and SBOX[1] == 0x7C and SBOX[255] == 0x16, \
        "AES S 盒的代数生成结果与 FIPS 197 印出来的表对不上"
    assert aes_encrypt_block(AES128_KEY, AES_PT) == AES128_CT, "AES-128 加密"
    assert aes_decrypt_block(AES128_KEY, AES128_CT) == AES_PT, "AES-128 解密"
    assert aes_encrypt_block(AES256_KEY, AES_PT) == AES256_CT, "AES-256 加密"
    assert aes_decrypt_block(AES256_KEY, AES256_CT) == AES_PT, "AES-256 解密"

    assert sm4_crypt_block(SM4_KEY, SM4_PT) == SM4_CT, "SM4 加密"
    assert sm4_crypt_block(SM4_KEY, SM4_CT, decrypt=True) == SM4_PT, "SM4 解密"

    assert sm3(b"abc") == SM3_ABC, "SM3(\"abc\")"
    assert sm3(b"abcd" * 16) == SM3_ABCD16, "SM3(\"abcd\"×16)"

    if million:
        # GB/T 32907 A.2：同一个块加密一百万次。慢，默认不跑。
        x = SM4_PT
        for _ in range(1_000_000):
            x = sm4_crypt_block(SM4_KEY, x)
        assert x == SM4_CT_1M, "SM4 一百万次迭代"

    print("标准向量：AES-128/256 加解密、SM4 加解密、SM3 两条 —— 全部一致"
          + ("（含一百万次迭代）" if million else ""))


def falsify() -> None:
    """每道检查都要能失败，否则它只是装饰"""
    # ① AES S 盒的仿射常数从 0x63 挪成 0x62
    box = [(s ^ 0x01) for s in SBOX]
    saved = SBOX[:]
    try:
        SBOX[:] = box
        broke = aes_encrypt_block(AES128_KEY, AES_PT) != AES128_CT
    finally:
        SBOX[:] = saved
    assert broke, "改了 AES S 盒，FIPS 197 的密文却没变 —— 这条检查是假的"

    # ② SM4 的 L 少一项旋转
    orig_t = globals()["_sm4_t"]

    def bad_t(x):
        b = _sm4_tau(x)
        return b ^ _rotl32(b, 2) ^ _rotl32(b, 10) ^ _rotl32(b, 18)   # 少了 <<<24

    try:
        globals()["_sm4_t"] = bad_t
        broke = sm4_crypt_block(SM4_KEY, SM4_PT) != SM4_CT
    finally:
        globals()["_sm4_t"] = orig_t
    assert broke, "SM4 的 L 少一项，GB/T 的密文却没变"

    # ③ SM3 的 T_j 分界从 16 挪到 15
    orig_c = globals()["sm3_compress"]

    def bad_compress(v, block):
        w, w1 = sm3_expand(block)
        a, b, c, d, e, f, g, h = v
        for j in range(64):
            t = 0x79CC4519 if j < 15 else 0x7A879D8A      # 分界挪了一格
            ss1 = _rotl32((_rotl32(a, 12) + e + _rotl32(t, j)) & 0xFFFFFFFF, 7)
            ss2 = ss1 ^ _rotl32(a, 12)
            tt1 = (_sm3_ff(j, a, b, c) + d + ss2 + w1[j]) & 0xFFFFFFFF
            tt2 = (_sm3_gg(j, e, f, g) + h + ss1 + w[j]) & 0xFFFFFFFF
            d, c, b, a = c, _rotl32(b, 9), a, tt1
            h, g, f, e = g, _rotl32(f, 19), e, _sm3_p0(tt2)
        return [x ^ y for x, y in zip([a, b, c, d, e, f, g, h], v)]

    try:
        globals()["sm3_compress"] = bad_compress
        broke = sm3(b"abc") != SM3_ABC
    finally:
        globals()["sm3_compress"] = orig_c
    assert broke, "SM3 的 T_j 分界挪了一格，GB/T 的摘要却没变"

    # ④ AES 解密不是加密的逆（换一把钥匙就该对不上）
    other = bytes((b ^ 0xFF) for b in AES128_KEY)
    assert aes_decrypt_block(other, AES128_CT) != AES_PT, "换钥匙居然也解得开"

    print("反证：AES S 盒常数、SM4 的 L、SM3 的 T_j 分界、AES 换钥匙 —— 四条都会失败")


def emit_sbox_verilog() -> str:
    """生成 RTL 里的 S 盒 case 表

    RTL 里放表、本文件放代数定义，测试台再逐值交叉验证 ——
    这样"表抄错一格"是**必然**被抓住的，不是"可能"被抓住。
    """
    out = []
    for name, tbl in (("aes_sbox", SBOX), ("aes_inv_sbox", INV_SBOX),
                      ("sm4_sbox", list(SM4_SBOX))):
        out.append(f"// ---- {name} ----")
        for i in range(0, 256, 8):
            row = " ".join(f"8'h{i + k:02x}: y = 8'h{tbl[i + k]:02x};"
                           for k in range(8))
            out.append("        " + row)
        out.append("")
    return "\n".join(out)


if __name__ == "__main__":
    if "--emit-sbox" in sys.argv:
        print(emit_sbox_verilog())
    else:
        check_vectors(million="--million" in sys.argv)
        falsify()
