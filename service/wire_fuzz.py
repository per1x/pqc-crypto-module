#!/usr/bin/env python3
"""对 pqchsm_fpgad 的线协议打畸形请求，看它会不会卡住 / 算错 / 崩

    python3 wire_fuzz.py <板子IP> <口令> [端口]

============================================================================
【判据不是"它拒绝了"，而是三条更强的】
============================================================================
一个只检查"非法请求返回错误码"的测试很容易通过，也很容易毫无价值 ——
真正会伤到演示的是另外三件事：

  ① **服务不能卡住。** 单线程服务被一条畸形请求挂住，后面所有人一起完蛋。
     所以每条用例都带超时，超时就是失败，而不是"等等看"。

  ② **服务不能死。** 每打完一发都重连一次做健康检查（PING + 一次真运算），
     确认它还在正常干活。只看当前这条请求的响应是看不出进程已经半死的。

  ③ **不能算出别的东西。** 最坏的失败不是报错，是**悄悄按另一个算法/另一把
     密钥算完并返回一个看着完全正常的结果**。RTL 对 alg 和槽号是静默截断的
     （sym_axi 里 `alg <= wr_data[1:0]`），所以 alg=4 会变成 AES-128、
     alg=7 会变成 SM3。这类用例单独列出来，判据是"必须被拒"，
     而不是"返回了什么都行"。

============================================================================
【为什么用裸 socket 而不是 libsdfe】
============================================================================
libsdfe 自己会做参数检查 —— 用它就永远送不出畸形请求，测的是客户端不是服务端。
这里直接按 wire.h 拼字节，绕过客户端的一切善意。
"""
import socket
import struct
import sys

MAGIC = 0x53434750
# ⚠️ 必须与 wire.h 的 PQCS_MAXPAY 一致。对不上不会报错，而是让下面那条
#    "len 超过 MAXPAY" 变成一条**送得进去的合法请求** —— 用例照常"通过"，
#    实际上已经不再测它声称要测的东西。这是双份定义最典型的坏法。
MAXPAY = 16384
(OP_PING, OP_RANDOM, OP_KEYGEN, OP_ENCAPS,
 OP_DECAPS, OP_IMPORT, OP_SYM, OP_AUTH,
 OP_MLDSA_KEYGEN, OP_MLDSA_SIGN, OP_MLDSA_VERIFY) = range(1, 12)

SDR_OK = 0
SDR_INARGERR = 0x01000004
SDR_KEYNOTEXIST = 0x01000005
SDR_HARDFAIL = 0x01000006
SDR_VERIFYFAIL = 0x01000008

TIMEOUT = 15.0          # 超过就算它卡住了

n_pass = n_fail = 0


def ok(m):
    global n_pass
    n_pass += 1
    print(f"  PASS  {m}")


def bad(m):
    global n_fail
    n_fail += 1
    print(f"  FAIL  {m}")


def connect(host, port, token):
    s = socket.create_connection((host, port), TIMEOUT)
    s.settimeout(TIMEOUT)
    tb = token.encode()
    s.sendall(struct.pack("<5I", MAGIC, OP_AUTH, 0, 0, len(tb)) + tb)
    hdr = s.recv(12)
    if len(hdr) != 12 or struct.unpack("<3I", hdr)[1] != SDR_OK:
        raise RuntimeError("认证失败")
    return s


def call(s, op, a0=0, a1=0, payload=b""):
    """返回 (status, data)。超时/断开抛异常 —— 那本身就是失败。"""
    s.sendall(struct.pack("<5I", MAGIC, op, a0, a1, len(payload)) + payload)
    hdr = b""
    while len(hdr) < 12:
        c = s.recv(12 - len(hdr))
        if not c:
            raise ConnectionError("服务端断开")
        hdr += c
    magic, status, ln = struct.unpack("<3I", hdr)
    if magic != MAGIC:
        raise RuntimeError(f"magic 不对: {magic:#x}")
    data = b""
    while len(data) < ln:
        c = s.recv(ln - len(data))
        if not c:
            raise ConnectionError("读载荷时断开")
        data += c
    return status, data


def healthy(host, port, token):
    """健康检查：连得上、PING 有回、而且**真的还能算** —— 只 PING 不够，
    PING 不碰密码核，一个硬件通路已经断掉的服务照样 PING 得通。"""
    try:
        s = connect(host, port, token)
        st, d = call(s, OP_PING)
        if st != SDR_OK or not d:
            return False
        st, d = call(s, OP_RANDOM, 32)
        s.close()
        return st == SDR_OK and len(d) == 32
    except Exception:
        return False


def case(host, port, token, name, fn, expect_reject=True):
    """打一发，然后独立重连做健康检查。"""
    global n_fail
    try:
        s = connect(host, port, token)
    except Exception as e:
        bad(f"{name}：连不上（{e}）")
        return
    try:
        st, d = fn(s)
        if expect_reject and st == SDR_OK:
            bad(f"{name}：**被接受了**（status=0，返回 {len(d)} 字节）—— "
                f"畸形输入不该算出结果")
        else:
            ok(f"{name}：status=0x{st:08x}，服务没卡住")
    except socket.timeout:
        bad(f"{name}：**超时 {TIMEOUT}s** —— 服务被这条请求挂住了")
        return
    except ConnectionError:
        # 断开本身不算失败（服务端可以选择断开非法连接），
        # 但下面的健康检查必须过：断开 ≠ 死掉。
        ok(f"{name}：服务端断开连接")
    except Exception as e:
        bad(f"{name}：异常 {e}")
    finally:
        try:
            s.close()
        except Exception:
            pass

    if not healthy(host, port, token):
        bad(f"{name}：打完之后服务不健康了 —— 这条请求伤到了它")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    host, token = sys.argv[1], sys.argv[2]
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 9797

    print("=== 线协议畸形输入 ===\n")
    if not healthy(host, port, token):
        sys.exit("开始之前服务就不健康，先修那个")

    print("[一] 参数越界 —— 必须拒绝")
    for nm, fn in [
        ("OP_RANDOM 长度 0",        lambda s: call(s, OP_RANDOM, 0)),
        ("OP_RANDOM 长度 0xFFFFFFFF", lambda s: call(s, OP_RANDOM, 0xFFFFFFFF)),
        ("OP_RANDOM 长度 MAXPAY+1", lambda s: call(s, OP_RANDOM, MAXPAY + 1)),
        ("KEYGEN pset=3（只有 0/1/2）", lambda s: call(s, OP_KEYGEN, 3)),
        ("KEYGEN pset=0xFFFFFFFF",  lambda s: call(s, OP_KEYGEN, 0xFFFFFFFF)),
        ("DECAPS 句柄 999（不存在）", lambda s: call(s, OP_DECAPS, 999, 0, b"\0" * 1088)),
        ("IMPORT_KEY 槽 99",        lambda s: call(s, OP_IMPORT, 99, 0, b"\0" * 16)),
        ("IMPORT_KEY 空密钥",       lambda s: call(s, OP_IMPORT, 3, 0, b"")),
        ("IMPORT_KEY 33 字节",      lambda s: call(s, OP_IMPORT, 3, 0, b"\0" * 33)),
        ("未知操作码 op=99",        lambda s: call(s, 99)),
        ("未知操作码 op=0",         lambda s: call(s, 0)),
        # ML-DSA：从机（mldsa_axi）尚未落地，所以这几条今天要么是参数被挡下、
        # 要么是硬件失败 —— 两种都算"拒绝"。它们真正盯的是**参数校验不会被
        # 跳过**：等从机上来之后，这批用例一个字都不用改就继续有效。
        ("ML-DSA KEYGEN pset=3（只有 0/1/2）",
         lambda s: call(s, OP_MLDSA_KEYGEN, 3)),
        ("ML-DSA KEYGEN pset=0xFFFFFFFF",
         lambda s: call(s, OP_MLDSA_KEYGEN, 0xFFFFFFFF)),
        ("ML-DSA SIGN 句柄 999（不存在）",
         lambda s: call(s, OP_MLDSA_SIGN, 999, 0, b"m")),
        ("ML-DSA SIGN 非空 ctx（约定里没给 ctx 字节留位置）",
         lambda s: call(s, OP_MLDSA_SIGN, 0, 8, b"m")),
        ("ML-DSA VERIFY pset=9",
         lambda s: call(s, OP_MLDSA_VERIFY, 9, 0, b"\0" * 100)),
        ("ML-DSA VERIFY 载荷短于 pk+sig",
         lambda s: call(s, OP_MLDSA_VERIFY, 0, 0, b"\0" * 100)),
    ]:
        case(host, port, token, nm, fn)

    print("\n[二] 长度与载荷对不上 —— 必须拒绝")
    for nm, fn in [
        ("ENCAPS ek 少一字节", lambda s: call(s, OP_ENCAPS, 1, 0, b"\0" * 1183)),
        ("ENCAPS ek 多一字节", lambda s: call(s, OP_ENCAPS, 1, 0, b"\0" * 1185)),
        ("ENCAPS 空载荷",      lambda s: call(s, OP_ENCAPS, 1, 0, b"")),
        ("SYM_BLOCK 15 字节",  lambda s: call(s, OP_SYM, 2, 3, b"\0" * 15)),
        ("SYM_BLOCK 17 字节",  lambda s: call(s, OP_SYM, 2, 3, b"\0" * 17)),
    ]:
        case(host, port, token, nm, fn)

    print("\n[三] 会被 RTL 静默截断的值 —— 拒绝，而不是算成别的算法")
    print("     （sym_axi 里 alg 只取低 2 位、槽号 3 位）")
    for nm, fn in [
        ("SYM_BLOCK alg=3（RTL 会当成 SM3）", lambda s: call(s, OP_SYM, 3, 3, b"\0" * 16)),
        ("SYM_BLOCK alg=4（截断成 AES-128）", lambda s: call(s, OP_SYM, 4, 3, b"\0" * 16)),
        ("SYM_BLOCK alg=7（截断成 SM3）",     lambda s: call(s, OP_SYM, 7, 3, b"\0" * 16)),
        ("SYM_BLOCK 槽=200（截断成槽 0）",    lambda s: call(s, OP_SYM, 2, 200, b"\0" * 16)),
        ("SYM_BLOCK 槽=8（只有 0..7）",       lambda s: call(s, OP_SYM, 2, 8, b"\0" * 16)),
    ]:
        case(host, port, token, nm, fn)

    print("\n[四] 协议层畸形 —— 服务可以断开，但不能死")
    for nm, fn in [
        ("magic 不对", lambda s: (s.sendall(struct.pack("<5I", 0xDEADBEEF, OP_PING, 0, 0, 0)),
                                  call(s, OP_PING))[1]),
        ("len 超过 MAXPAY", lambda s: (s.sendall(struct.pack("<5I", MAGIC, OP_RANDOM, 32, 0, MAXPAY + 1)),
                                       call(s, OP_PING))[1]),
        ("声明 100 字节只发 10 字节（半条请求）",
         lambda s: (s.sendall(struct.pack("<5I", MAGIC, OP_IMPORT, 3, 0, 100) + b"\0" * 10),
                    call(s, OP_PING))[1]),
    ]:
        case(host, port, token, nm, fn)

    print("\n[五] 收尾：服务还能正常干活吗")
    try:
        s = connect(host, port, token)
        st, d = call(s, OP_RANDOM, 32)
        ok(f"OP_RANDOM 32 字节 status={st} len={len(d)}") if st == SDR_OK and len(d) == 32 \
            else bad(f"收尾的正常请求失败 status=0x{st:08x}")
        st, d = call(s, OP_KEYGEN, 1)
        ok(f"KEYGEN pset=1 返回 {len(d)} 字节（4 句柄 + 1184 ek）") \
            if st == SDR_OK and len(d) == 1188 else bad(f"KEYGEN 失败 0x{st:08x} len={len(d)}")
        s.close()
    except Exception as e:
        bad(f"收尾失败：{e}")

    print(f"\n{'='*58}\n通过 {n_pass}，失败 {n_fail}")
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
