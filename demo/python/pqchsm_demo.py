#!/usr/bin/env python3
"""Python 端到端 demo：把 pqc-hsm 当作 PKCS#11 provider 调用

【为什么用 PyKCS11 而不是 python-pkcs11】
python-pkcs11 是高层封装，机制与属性都是它自己的枚举 —— **它还没有适配
ML-KEM / ML-DSA**，新机制码传不进去。PyKCS11 是 SWIG 直接封装 C API，
机制码和属性类型都可以直接给整数，所以能调任意 CKM_ / CKA_，包括 v3.2 才有的
CKM_ML_DSA(0x1d)、CKM_ML_KEM(0x17)、CKA_PARAMETER_SET(0x61d)。

这不是"绕过"，而是当前唯一走得通的路：高层封装适配新算法总要滞后一截。

用法：
    .venv-p11/bin/python demo/python/pqchsm_demo.py [模块路径]
"""
from __future__ import annotations

import os
import sys
import tempfile

import PyKCS11
from PyKCS11 import PyKCS11Error
from PyKCS11.LowLevel import CKA_CLASS  # noqa: F401  （确认 LowLevel 可用）

# ---- PKCS#11 v3.2 的常量（PyKCS11 自带的枚举里还没有，直接用数值）----------
CKM_ML_KEM_KEY_PAIR_GEN = 0x0000000F
CKM_ML_KEM = 0x00000017
CKM_ML_DSA_KEY_PAIR_GEN = 0x0000001C
CKM_ML_DSA = 0x0000001D

CKK_ML_KEM = 0x00000049
CKK_ML_DSA = 0x0000004A

CKA_PARAMETER_SET = 0x0000061D
CKA_ENCAPSULATE = 0x00000633
CKA_DECAPSULATE = 0x00000634

CKP_ML_DSA_44, CKP_ML_DSA_65, CKP_ML_DSA_87 = 1, 2, 3
CKP_ML_KEM_512, CKP_ML_KEM_768, CKP_ML_KEM_1024 = 1, 2, 3


def register_pqc_constants() -> None:
    """把 PQC 常量注册进 PyKCS11 的名字表。

    ⚠️ **这一步是必需的，也正是"高层封装还没适配新算法"的确切位置。**
    PyKCS11 的 CKM / CKA / CKK 是普通 Python dict（名字↔数值双向），
    里面没有任何 ML-KEM / ML-DSA 条目。不注册的话：

        lib.getMechanismList(slot)  →  KeyError: 28

    因为它拿到机制码 0x1C 后去 CKM 里反查名字，查不到就抛。
    注册之后高层 API 就能正常工作 —— 说明缺的只是常量表，不是能力。
    """
    for name, val in (
        ("CKM_ML_KEM_KEY_PAIR_GEN", CKM_ML_KEM_KEY_PAIR_GEN),
        ("CKM_ML_KEM", CKM_ML_KEM),
        ("CKM_ML_DSA_KEY_PAIR_GEN", CKM_ML_DSA_KEY_PAIR_GEN),
        ("CKM_ML_DSA", CKM_ML_DSA),
    ):
        PyKCS11.CKM[name] = val
        PyKCS11.CKM[val] = name
    for name, val in (("CKK_ML_KEM", CKK_ML_KEM), ("CKK_ML_DSA", CKK_ML_DSA)):
        PyKCS11.CKK[name] = val
        PyKCS11.CKK[val] = name
    for name, val in (("CKA_PARAMETER_SET", CKA_PARAMETER_SET),
                      ("CKA_ENCAPSULATE", CKA_ENCAPSULATE),
                      ("CKA_DECAPSULATE", CKA_DECAPSULATE)):
        PyKCS11.CKA[name] = val
        PyKCS11.CKA[val] = name


def ulong_bytes(v: int) -> list:
    """把一个 CK_ULONG 编成字节列表。

    ⚠️ 适配缺口的第三处：PyKCS11 靠内部类型表决定一个属性该用
    SetNum / SetBool / SetBin 写入；CKA_PARAMETER_SET 不在表里，它默认走 SetBin，
    直接传 int 会报
        TypeError: argument 3 of type 'std::vector<unsigned char> const &'
    所以这里自己按 CK_ULONG（64 位平台上 8 字节，小端）编好再传。
    """
    return list(v.to_bytes(8, "little"))


def get_num(sess, obj, attr: int) -> int:
    """读一个数值属性。

    PyKCS11 判断"这个属性是数字还是布尔还是字节串"依赖 SWIG 层的类型表，
    新属性它同样不认，所以这里统一用 allAsBinary 取原始字节自己解 —— 
    小端 CK_ULONG。
    """
    raw = bytes(sess.getAttributeValue(obj, [attr], allAsBinary=True)[0])
    return int.from_bytes(raw, "little")

SO_PIN = "12345678"
USER_PIN = "1234abcd"

ok = 0
bad = 0


def check(cond: bool, what: str) -> None:
    global ok, bad
    if cond:
        ok += 1
        print(f"  ✓ {what}")
    else:
        bad += 1
        print(f"  ✗ {what}")


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, "..", ".."))
    # 共享库后缀随平台变：Linux 是 .so，macOS 是 .dylib。默认路径两个都试，
    # 免得在另一个平台上得到"找不到模块"却看不出是后缀不对。
    if len(sys.argv) > 1:
        module = sys.argv[1]
    else:
        candidates = [os.path.join(root, "build", "pqchsm-pkcs11" + ext)
                      for ext in (".so", ".dylib")]
        module = next((c for c in candidates if os.path.exists(c)), candidates[0])
    if not os.path.exists(module):
        print(f"找不到模块 {module} —— 先 cmake --build build --target pqchsm-p11")
        print("（Linux 上是 build/pqchsm-pkcs11.so，macOS 上是 .dylib；"
              "也可以把路径作为第一个参数传进来）")
        return 2

    # 每次跑用独立密钥库，避免与别的 demo 互相影响
    ks = os.path.join(tempfile.mkdtemp(), "keystore.bin")
    os.environ["PQCHSM_KEYSTORE"] = ks
    os.environ["PQCHSM_SLOTS"] = "3"

    print(f"模块   : {module}")
    print(f"密钥库 : {ks}")
    print()

    register_pqc_constants()

    lib = PyKCS11.PyKCS11Lib()
    lib.load(module)

    print("== 1. 库信息与机制列表 ==")
    info = lib.getInfo()
    print(f"  Cryptoki {info.cryptokiVersion[0]}.{info.cryptokiVersion[1]}"
          f"  厂商 {info.manufacturerID.strip()}")
    check(info.cryptokiVersion[0] == 3 and info.cryptokiVersion[1] == 2,
          "Cryptoki 版本是 3.2")

    slots = lib.getSlotList(tokenPresent=True)
    check(len(slots) == 3, f"槽位数 = {len(slots)}")
    slot = slots[0]

    mechs = lib.getMechanismList(slot)
    # 注册之后 getMechanismList 返回的是**名字**（它内部做了 码→名 反查），
    # 所以这里再映射回数值
    mech_vals = {PyKCS11.CKM[m] if isinstance(m, str) else int(m) for m in mechs}
    print(f"  机制：{sorted(str(m) for m in mechs)}")
    check(CKM_ML_DSA in mech_vals, f"机制列表含 CKM_ML_DSA (0x{CKM_ML_DSA:x})")
    check(CKM_ML_KEM in mech_vals, f"机制列表含 CKM_ML_KEM (0x{CKM_ML_KEM:x})")

    print("\n== 2. 初始化 token 与 PIN ==")
    lib.initToken(slot, SO_PIN, "pyDemo")
    check(True, "C_InitToken")
    so_sess = lib.openSession(slot, PyKCS11.CKF_SERIAL_SESSION | PyKCS11.CKF_RW_SESSION)
    so_sess.login(SO_PIN, user_type=PyKCS11.CKU_SO)
    so_sess.initPin(USER_PIN)
    check(True, "C_InitPIN（SO 会话）")
    so_sess.logout()
    so_sess.closeSession()

    print("\n== 3. ML-DSA-65：生成密钥对 → 签名 → 验签 ==")
    sess = lib.openSession(slot, PyKCS11.CKF_SERIAL_SESSION | PyKCS11.CKF_RW_SESSION)
    sess.login(USER_PIN)
    check(True, "C_Login(CKU_USER)")

    # 关键点：机制码与 CKA_PARAMETER_SET 都直接给整数
    pub_tmpl = [(CKA_PARAMETER_SET, ulong_bytes(CKP_ML_DSA_65))]
    mech = PyKCS11.Mechanism(CKM_ML_DSA_KEY_PAIR_GEN, None)
    pub, priv = sess.generateKeyPair(pub_tmpl, [], mecha=mech)
    check(pub is not None and priv is not None, "C_GenerateKeyPair(CKM_ML_DSA_KEY_PAIR_GEN)")

    kt = get_num(sess, priv, PyKCS11.CKA_KEY_TYPE)
    check(kt == CKK_ML_DSA, f"私钥 CKA_KEY_TYPE = CKK_ML_DSA (0x{kt:x})")
    pset = get_num(sess, priv, CKA_PARAMETER_SET)
    check(pset == CKP_ML_DSA_65, f"CKA_PARAMETER_SET = CKP_ML_DSA_65 ({pset})")

    # HSM 的意义：私钥的 CKA_VALUE 必须取不出来。
    # 注意 PyKCS11 的行为：它收到 CKR_ATTRIBUTE_SENSITIVE 不会抛异常，
    # 而是降级到分片查询、最终把该属性返回成 None —— 所以判据是"值为 None"。
    priv_val = sess.getAttributeValue(priv, [PyKCS11.CKA_VALUE])[0]
    check(priv_val is None, "私钥 CKA_VALUE 取不出来（模块返回 CKR_ATTRIBUTE_SENSITIVE）")
    pub_val_probe = sess.getAttributeValue(pub, [PyKCS11.CKA_VALUE])[0]
    check(pub_val_probe is not None, "对照：公钥 CKA_VALUE 可以取出来")

    pk_bytes = bytes(sess.getAttributeValue(pub, [PyKCS11.CKA_VALUE])[0])
    check(len(pk_bytes) == 1952, f"公钥长度 {len(pk_bytes)} B（ML-DSA-65 应为 1952）")

    data = b"hello from python via PKCS#11"
    sig = bytes(sess.sign(priv, data, PyKCS11.Mechanism(CKM_ML_DSA, None)))
    check(len(sig) == 3309, f"签名长度 {len(sig)} B（ML-DSA-65 应为 3309）")

    verified = sess.verify(pub, data, sig, PyKCS11.Mechanism(CKM_ML_DSA, None))
    check(verified, "C_Verify 通过")

    tampered = bytearray(data)
    tampered[0] ^= 0x01
    bad_verify = sess.verify(pub, bytes(tampered), sig, PyKCS11.Mechanism(CKM_ML_DSA, None))
    check(not bad_verify, "篡改消息后验签失败（负测试）")

    print("\n== 4. C_FindObjects：应当看到一个公钥 + 一个私钥 ==")
    objs = sess.findObjects()
    check(len(objs) == 2, f"找到 {len(objs)} 个对象")
    privs = sess.findObjects([(PyKCS11.CKA_CLASS, PyKCS11.CKO_PRIVATE_KEY)])
    check(len(privs) == 1, f"按 CKA_CLASS 过滤私钥：{len(privs)} 个")
    sess.logout()
    sess.closeSession()

    print("\n== 5. ML-KEM-768：生成密钥对并读回属性 ==")
    slot2 = slots[1]
    lib.initToken(slot2, SO_PIN, "pyKem")
    s2 = lib.openSession(slot2, PyKCS11.CKF_SERIAL_SESSION | PyKCS11.CKF_RW_SESSION)
    s2.login(SO_PIN, user_type=PyKCS11.CKU_SO)
    s2.initPin(USER_PIN)
    s2.logout()
    s2.login(USER_PIN)
    kmech = PyKCS11.Mechanism(CKM_ML_KEM_KEY_PAIR_GEN, None)
    kpub, kpriv = s2.generateKeyPair(
        [(CKA_PARAMETER_SET, ulong_bytes(CKP_ML_KEM_768))], [], mecha=kmech)
    check(kpub is not None, "C_GenerateKeyPair(CKM_ML_KEM_KEY_PAIR_GEN)")
    kkt = get_num(s2, kpriv, PyKCS11.CKA_KEY_TYPE)
    check(kkt == CKK_ML_KEM, f"CKA_KEY_TYPE = CKK_ML_KEM (0x{kkt:x})")
    dec = get_num(s2, kpriv, CKA_DECAPSULATE)
    check(bool(dec), "私钥 CKA_DECAPSULATE = true")
    kpk = bytes(s2.getAttributeValue(kpub, [PyKCS11.CKA_VALUE])[0])
    check(len(kpk) == 1184, f"ML-KEM-768 公钥 {len(kpk)} B（应为 1184）")

    # 用途互斥：KEM 私钥不能拿去签名
    usage_blocked = False
    try:
        s2.sign(kpriv, b"x", PyKCS11.Mechanism(CKM_ML_DSA, None))
    except PyKCS11Error:
        usage_blocked = True
    check(usage_blocked, "KEM 私钥拿去签名被拒（用途互斥）")
    s2.logout()
    s2.closeSession()

    print("\n== 6. 跨进程持久化：重新 Initialize 后密钥还在 ==")
    lib2 = PyKCS11.PyKCS11Lib()
    lib2.load(module)
    s3 = lib2.openSession(slots[0], PyKCS11.CKF_SERIAL_SESSION)
    s3.login(USER_PIN)
    objs2 = s3.findObjects()
    check(len(objs2) == 2, f"重新加载后 slot0 仍有 {len(objs2)} 个对象")
    # 用恢复出来的私钥再签一次，并用第一次拿到的公钥验证
    priv2 = s3.findObjects([(PyKCS11.CKA_CLASS, PyKCS11.CKO_PRIVATE_KEY)])[0]
    sig2 = bytes(s3.sign(priv2, data, PyKCS11.Mechanism(CKM_ML_DSA, None)))
    pub2 = s3.findObjects([(PyKCS11.CKA_CLASS, PyKCS11.CKO_PUBLIC_KEY)])[0]
    check(s3.verify(pub2, data, sig2, PyKCS11.Mechanism(CKM_ML_DSA, None)),
          "重新加载后签名/验签仍可用")
    s3.logout()
    s3.closeSession()

    print()
    print(f"通过 {ok}，失败 {bad}")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
