#!/usr/bin/env python3
"""serial_console.py —— 从 Mac 经串口驱动板子（掉网时的**首选**兜底）

    python3 serial_console.py "命令"          # 登录并跑一条命令，回显结果
    python3 serial_console.py --watch 90      # 只收，不发（看启动日志用）
    python3 serial_console.py --raw "字符串"   # 原样发一段，不做登录

============================================================================
【为什么它比 JTAG 优先】
============================================================================
这块板有三条进得去的路，可靠性从高到低：

  ① **串口**：接在 PS 的 UART0（ttyPS0）上，**完全不依赖 PL、不依赖网络**。
     密码位流一装 eth0 就没了、eth1 也可能掉，但串口一直在。
  ② 网络：eth1（PS 的 GEM 硬核）能穿过 PL 重配活下来，但依赖 PHY 与配置。
  ③ JTAG：能引导内核、能读写内存，但**做不到 POR**，而且救砖路径上每加一层
     依赖（PMUFW/BL31/console/psci）就多一处能挂住的地方。

⚠️ **以前的判断是错的，别再照着走。** 仓库里一度记着"串口在 USB 层坏掉、
   只能拔插"，于是掉网就直奔 JTAG 或者请人断电。实际情况是：CP2102N 偶尔会
   进入一种**枚举正常但控制端点全废**的状态（`tcsetattr` 一律 EINVAL、
   一个字节都收不到），拔插那根线就好。**先拔插串口线，再考虑 JTAG。**

============================================================================
【怎么判断串口是好的】
============================================================================
    stty -f /dev/cu.usbserial-110 115200 cs8 -cstopb -parenb
不报错就是好的；报 `tcsetattr: Invalid argument` 就是上面那个坏状态，拔插。
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("缺 pyserial：python3 -m pip install pyserial")

DEV = "/dev/cu.usbserial-110"
BAUD = 115200
USER = "root"
PASS = "root"


def drain(s, secs=0.6):
    out = b""
    t0 = time.time()
    while time.time() - t0 < secs:
        d = s.read(4096)
        if d:
            out += d
            t0 = time.time()          # 还在吐就继续等
    return out


def ensure_login(s, timeout=25):
    """把会话弄到一个 shell 提示符上。已经登录过就直接回来。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        s.write(b"\r\n")
        s.flush()
        buf = drain(s, 1.2).decode("utf-8", "replace")
        if "login:" in buf.lower():
            s.write((USER + "\r\n").encode())
            s.flush()
            buf = drain(s, 2.0).decode("utf-8", "replace")
            if "password" in buf.lower():
                s.write((PASS + "\r\n").encode())
                s.flush()
                drain(s, 2.5)
            continue
        if "assword" in buf:
            s.write((PASS + "\r\n").encode())
            s.flush()
            drain(s, 2.5)
            continue
        # 提示符长这样：root@petalinux:~#
        if "#" in buf or "$" in buf:
            return True
    return False


def run(cmd, secs=12):
    s = serial.Serial(DEV, BAUD, timeout=0.3)
    try:
        if not ensure_login(s):
            print("!!! 没能拿到 shell 提示符（板子可能还没起来，或者串口是坏状态）")
            return 2
        # 加一个哨兵，好知道命令什么时候跑完
        s.write((cmd + " ; echo __DONE__$?\r\n").encode())
        s.flush()
        out = b""
        t0 = time.time()
        while time.time() - t0 < secs:
            d = s.read(4096)
            if d:
                out += d
                if b"__DONE__" in out:
                    break
        sys.stdout.write(out.decode("utf-8", "replace"))
        return 0
    finally:
        s.close()


def watch(secs):
    s = serial.Serial(DEV, BAUD, timeout=0.3)
    try:
        t0 = time.time()
        while time.time() - t0 < secs:
            d = s.read(4096)
            if d:
                sys.stdout.write(d.decode("utf-8", "replace"))
                sys.stdout.flush()
    finally:
        s.close()
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", nargs="?", help="要在板子上跑的命令")
    ap.add_argument("--watch", type=float, help="只收多少秒（看启动日志）")
    ap.add_argument("--raw", help="原样发一段，不做登录")
    ap.add_argument("--secs", type=float, default=12, help="等命令输出多少秒")
    a = ap.parse_args()
    if a.watch:
        return watch(a.watch)
    if a.raw is not None:
        s = serial.Serial(DEV, BAUD, timeout=0.3)
        s.write(a.raw.encode())
        s.flush()
        sys.stdout.write(drain(s, 3).decode("utf-8", "replace"))
        s.close()
        return 0
    if not a.cmd:
        ap.error("给一条命令，或者用 --watch")
    return run(a.cmd, a.secs)


if __name__ == "__main__":
    sys.exit(main())
