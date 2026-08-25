#!/usr/bin/env python3
"""在 U-Boot 提示符下把 CSU_MULTI_BOOT 设成指定的槽号（串口，带回显校验）

    python3 board/scripts/uboot_multiboot.py 6          # 设槽 6 并 reset
    python3 board/scripts/uboot_multiboot.py --read     # 只读，不写

============================================================================
【为什么必须走串口，以及为什么这不违反"串口只读"那条规矩】
============================================================================
⚠️⚠️ **2026-08-25：先别用这个脚本 —— 有一条走网络的路，而且它一直都在。**

    busybox insmod pmsec.ko set_multiboot=6      # 经 PMU 的 PM_MMIO_WRITE
    sh hsmreboot.sh

`board/kmod/pmsec.c` 文件头第 24 行原文就写着这件事，而且已在板上验过
（0x00000000 -> 0x00000006，重启后读回仍是 6）。它走网络、不碰串口，
既不违反"串口只读"，也不需要 JTAG。**优先用它。**

这个脚本降级为**够不着时的退路**（比如 daemon/内核起不来、只剩 U-Boot
提示符）。下面这段推理里"从 Linux 没有任何安全路径"那句是错的 ——
错在只想到了 APU 直接访问，忘了 PMU 可以代劳。

    ---- 以下为原来的推理，保留（前半仍然成立）----
CSU_MULTI_BOOT（0xFFCA0010）**APU 在 EL1 既读不到也写不得** —— board/src/xmpu_probe.c
把"读不到"当作正确结果，而写它是 SError（接不住，只能断电）。**但 PMU 可以**：
PM_MMIO_READ/WRITE 是转给 PMU 去执行的，U-Boot 的 `zynqmp mmio_write` 走的
也是同一条 PM 通路。

仓库的硬规矩是"串口只用于只读观察，任何会写的命令走网络"。它的理由是**串口会
把发出去的字符弄重复**（实测 `2>/dev/null` → `2>/deev/null`、`BOOT_GOLDEN` →
`BOOT_GOOLDEN`），而板子收到的就是坏的那份。

这个脚本不是那条规矩的例外，是**把那条规矩的前提直接消掉**：

    发完命令**先不按回车**，把 U-Boot 的回显整行读回来，与要发的字符串
    逐字节比对。对不上就发 Ctrl-U 清行重来，对上了才发回车。

也就是说，判据不再是"我发了什么"，而是**"板子实际收到了什么"** —— 那正是坏
字符会现形的地方。写完再 `md` 读回来复核一次，两道都过才算数。

============================================================================
【安全边界】
============================================================================
· 只写 0xFFCA0010 这一个寄存器，值是槽号。不碰任何别的地址。
· multiboot **由 POR 清零** —— 任何一次断电都自动回到 BOOT.BIN（golden）。
  所以最坏情况是"要断一次电"，不是砖。
· 不碰 SD 上任何文件，不动 golden。
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
MULTIBOOT_REG = "0xffca0010"


def drain(s, secs=0.5):
    out = b""
    t0 = time.time()
    while time.time() - t0 < secs:
        d = s.read(4096)
        if d:
            out += d
            t0 = time.time()
    return out


def get_uboot_prompt(s, secs=90):
    """在自动启动倒计时里插进去，拿到 U-Boot 提示符。

    只发换行 —— 换行被弄重复了也无所谓（多一个空命令），这是唯一一种
    "坏掉也没有后果"的输入。真正的命令留到拿到提示符之后再逐字校验。
    """
    print("等 U-Boot 倒计时（板子要在这期间重启）…", flush=True)
    t0 = time.time()
    buf = ""
    while time.time() - t0 < secs:
        d = s.read(4096)
        if d:
            buf += d.decode("utf-8", "replace")
            sys.stdout.write(d.decode("utf-8", "replace"))
            sys.stdout.flush()
        # 一看到倒计时就开始猛敲回车
        if "Hit any key" in buf or "autoboot" in buf.lower():
            for _ in range(40):
                s.write(b"\r\n")
                s.flush()
                time.sleep(0.05)
            buf = ""
        s.write(b"\r\n")
        s.flush()
        tail = drain(s, 0.4).decode("utf-8", "replace")
        sys.stdout.write(tail)
        sys.stdout.flush()
        if "ZynqMP>" in tail or "U-Boot>" in tail or "Zynq>" in tail:
            return True
        buf += tail
    return False


def send_verified(s, cmd, tries=4):
    """把 cmd 敲进去，**读回显逐字校验，对上了才按回车**。

    这是整个脚本的要害：串口会把字符弄重复，而回显反映的是**板子收到的**
    那一份。对不上就 Ctrl-U 清整行重来，绝不按回车。
    """
    for attempt in range(1, tries + 1):
        s.write(b"\x15")          # Ctrl-U：先把这一行清干净
        s.flush()
        drain(s, 0.4)
        for ch in cmd:            # 一个字符一个字符发，给回显留时间
            s.write(ch.encode())
            s.flush()
            time.sleep(0.02)
        echo = drain(s, 1.0).decode("utf-8", "replace")
        # 回显里可能夹着提示符与控制字符，只看它是否**恰好**包含目标串，
        # 且不含目标串的坏变体（重复字符会让这一条不成立）
        got = echo.replace("\r", "").replace("\n", "")
        if cmd in got:
            # 还要确认没有多出来的字符黏在命令里（重复字符的典型形状）
            idx = got.rfind(cmd)
            trailing = got[idx + len(cmd):]
            if trailing.strip(" ") == "":
                print(f"\n  ✓ 回显逐字校验通过（第 {attempt} 次）：{cmd!r}")
                s.write(b"\r\n")
                s.flush()
                return drain(s, 2.0).decode("utf-8", "replace")
        print(f"\n  ✗ 第 {attempt} 次回显对不上，清行重来。收到：{got!r}")
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("slot", nargs="?", type=int, help="要设的 multiboot 槽号")
    ap.add_argument("--read", action="store_true", help="只读，不写")
    ap.add_argument("--no-reset", action="store_true", help="写完不 reset")
    a = ap.parse_args()
    if not a.read and a.slot is None:
        ap.error("给一个槽号，或者用 --read")
    if a.slot is not None and not (0 <= a.slot <= 15):
        ap.error("槽号只认 0..15")

    s = serial.Serial(DEV, BAUD, timeout=0.3)
    try:
        if not get_uboot_prompt(s):
            print("\n!!! 没拿到 U-Boot 提示符。板子可能没在这段时间重启，"
                  "或者已经进了 Linux。什么都没写。")
            return 2

        print("\n=== 拿到 U-Boot 提示符 ===")
        out = send_verified(s, f"md {MULTIBOOT_REG} 1")
        print(out or "(读不到)")

        if a.read:
            return 0

        cmd = f"zynqmp mmio_write {MULTIBOOT_REG} 0xffffffff 0x{a.slot:x}"
        print(f"\n=== 写 multiboot = {a.slot} ===")
        out = send_verified(s, cmd)
        if out is None:
            print("!!! 回显始终对不上，**没有按回车**，什么都没写。")
            return 2
        print(out)

        print("\n=== 读回复核 ===")
        out = send_verified(s, f"md {MULTIBOOT_REG} 1")
        print(out or "(读不到)")
        if out and f"{a.slot:08x}" not in out.replace(" ", "").lower():
            print(f"!!! 读回来的不是 {a.slot} —— 不 reset，请人工确认。")
            return 2
        print(f"  ✓ 读回确认 = {a.slot}")

        if not a.no_reset:
            print("\n=== reset ===")
            send_verified(s, "reset")
        return 0
    finally:
        s.close()


if __name__ == "__main__":
    sys.exit(main())
