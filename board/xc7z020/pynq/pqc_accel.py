#!/usr/bin/env python3
"""PYNQ 路径下的加速器驱动

这一层与 board/xc7z020/src/accel_zynq.c 是同一份寄存器契约的两种落地方式：
控制面对 AXI4-Lite 寄存器组做读写，数据面由 AXI-DMA 在 DDR 与 PL 之间搬运，
时序完全照 docs/register-map.md 的规定 —— 送数据、写 MODE、写 IN_LEN、
写 CTRL=START、轮询 STATUS.DONE、读 OUT_LEN、取回结果。

【为什么 PYNQ 与 C 两条路径都要有】
PYNQ 这条路径不需要交叉编译、不需要设备树，改一行 Python 就能重新试一次，
适合刚拿到比特流时确认"地址对不对、时钟通不通、结果对不对"。C 那条路径才是
产品形态（PetaLinux + /dev/mem 或 UIO），但它的每次改动都要重新交叉编译。
两条路径遵守同一份寄存器契约，因此 PYNQ 上验证过的时序在 C 侧同样成立。

【Cache 一致性】pynq.allocate 分配的是 dma-coherent（CMA）缓冲，
CPU 侧的访问不经过会与 DMA 别名的缓存，因此**不需要手工 flush/invalidate**。
PynqBuffer 上确实有 flush() 与 invalidate()，但在 coherent 缓冲上它们不做事，
本文件因此一次都不调用。这一点与 C 那条路径不同：那边用 /dev/mem + O_SYNC
把保留内存映射成非缓存，才得到同样的效果。

【依赖】只在真正需要硬件时才导入 pynq 与 numpy。开发机上没有这两个库时，
导入本模块仍然成功，模块里的纯 Python 函数（打包/解包）也照样可用；
只有构造 PqcAccel 时才会报出一句可读的说明。
"""
from __future__ import annotations

import time

# ---- 寄存器契约：与 docs/register-map.md、include/pqchsm/accel.h 一致 ----

ACCEL_BASE = 0x43C00000        # 与 vivado/create_project.tcl 的地址映射一致
ACCEL_SPAN = 0x00010000

REG_CTRL = 0x00
REG_STATUS = 0x04
REG_MODE = 0x08
REG_PARAM = 0x0C
REG_IN_LEN = 0x10
REG_OUT_LEN = 0x14
REG_ERRCODE = 0x18
REG_VERSION = 0x1C

CTRL_START = 1 << 0            # 自清，读回恒为 0
CTRL_SOFT_RESET = 1 << 1       # 自清

ST_DONE = 1 << 0               # 电平锁存，保持到下一次 START
ST_BUSY = 1 << 1
ST_ERR = 1 << 2

VERSION_EXPECTED = 0x00010000

MODE_NTT_FWD = 7
MODE_NTT_INV = 8
MODE_KECCAK_F1600 = 9

NTT_COEFFS = 256
NTT_LEN = 512                  # 256 个 16 位系数
KECCAK_LANES = 25
KECCAK_LEN = 200               # 25 个 64 位 lane

# 硬件侧的收发缓冲是 512 字节，输出不可能超过这个数，
# 因此接收通道一律按 512 字节武装，实际长度以 OUT_LEN 为准。
MAX_XFER = 512

ERRCODE_MODE_NOT_IMPLEMENTED = 3

_NO_PYNQ_MSG = (
    "导入 pynq / numpy 失败：{exc}\n"
    "本模块的硬件部分只能在装有 PYNQ 镜像的板子上运行（PYNQ-Z2 等 Zynq-7000 板）。\n"
    "开发机上无需安装：pqc_accel 里的打包/解包函数不依赖 pynq，可以直接导入使用；\n"
    "参考值自检见同目录 selftest.py 的 --refs-only。"
)


class PqcAccelError(RuntimeError):
    """加速器路径上的所有失败都归到这一类"""


class PqcAccelCommandError(PqcAccelError):
    """一条命令以 STATUS.ERR 结束，errcode 是硬件给出的细分原因"""

    def __init__(self, message: str, status: int, errcode: int):
        super().__init__(message)
        self.status = status
        self.errcode = errcode


class PqcAccelTimeout(PqcAccelError):
    """在给定时限内既没等到 DONE，也没等到 DMA 通道空闲"""


# ---- 打包与解包：不依赖 pynq，开发机上可直接使用 ----


def pack_coeffs(coeffs) -> bytes:
    """256 个系数 → 512 字节。每个系数 16 位有符号、小端，两个系数一个 32 位字。

    字节序与 hardware/tb/cocotb/test_axi.py 里的构造一致：低地址放偶数下标的
    系数，也就是 32 位字的低半字。
    """
    seq = list(coeffs)
    if len(seq) != NTT_COEFFS:
        raise ValueError(f"NTT 需要 {NTT_COEFFS} 个系数，收到 {len(seq)} 个")
    out = bytearray(NTT_LEN)
    for i, c in enumerate(seq):
        v = int(c)
        if not -0x8000 <= v <= 0x7FFF:
            raise ValueError(f"系数 {v} 超出 16 位有符号范围（下标 {i}）")
        out[2 * i] = v & 0xFF
        out[2 * i + 1] = (v >> 8) & 0xFF
    return bytes(out)


def unpack_coeffs(data: bytes) -> list[int]:
    """512 字节 → 256 个 16 位有符号系数，是 pack_coeffs 的逆"""
    if len(data) != NTT_LEN:
        raise ValueError(f"NTT 结果应为 {NTT_LEN} 字节，收到 {len(data)} 字节")
    out = []
    for i in range(NTT_COEFFS):
        v = data[2 * i] | (data[2 * i + 1] << 8)
        out.append(v - 0x10000 if v >= 0x8000 else v)
    return out


def bytes_to_lanes(data: bytes) -> list[int]:
    """200 字节 → 25 个 64 位 lane（小端）。给参考模型比对用。"""
    if len(data) != KECCAK_LEN:
        raise ValueError(f"Keccak 状态应为 {KECCAK_LEN} 字节，收到 {len(data)} 字节")
    return [int.from_bytes(data[8 * i:8 * i + 8], "little") for i in range(KECCAK_LANES)]


def lanes_to_bytes(lanes) -> bytes:
    """25 个 64 位 lane → 200 字节（小端），是 bytes_to_lanes 的逆"""
    seq = list(lanes)
    if len(seq) != KECCAK_LANES:
        raise ValueError(f"Keccak 状态应为 {KECCAK_LANES} 个 lane，收到 {len(seq)} 个")
    return b"".join((int(x) & ((1 << 64) - 1)).to_bytes(8, "little") for x in seq)


# ---- 依赖导入 ----


def _import_pynq():
    """把 pynq 的导入收在这里：缺库时给一句可读的说明，而不是抛 ImportError 栈"""
    try:
        import numpy as np
        from pynq import MMIO, Overlay, allocate
    except ImportError as exc:      # 开发机上没有 pynq 是正常情况
        raise PqcAccelError(_NO_PYNQ_MSG.format(exc=exc)) from None
    return np, Overlay, allocate, MMIO


# ---- 驱动 ----


class PqcAccel:
    """加载 overlay 并按寄存器契约驱动加速器

    参数
        bitfile     .bit 的路径。同目录下必须有同名的 .hwh，PYNQ 靠它拿到
                    地址映射与 IP 列表 —— 详见本目录 README。
        accel_name  BD 里加速器实例的名字，与 create_project.tcl 一致。
        dma_name    BD 里 AXI-DMA 实例的名字。
        timeout     单次等待的时限（秒）。既用于轮询 DONE，也用于等 DMA 通道空闲。
        overlay     已经加载好的 Overlay 对象。给"一个 overlay 上挂多个驱动"
                    的场景用；给了它就不再重新加载比特流。
    """

    def __init__(self, bitfile: str = "pqc_accel_bd_wrapper.bit", *,
                 accel_name: str = "pqc_accel_0",
                 dma_name: str = "axi_dma_0",
                 timeout: float = 1.0,
                 overlay=None):
        np, Overlay, allocate, MMIO = _import_pynq()
        self._np = np
        self._allocate = allocate
        self._timeout = float(timeout)

        self.overlay = Overlay(bitfile) if overlay is None else overlay

        base, span = self._resolve_accel(accel_name)
        self._regs = MMIO(base, span)
        self.accel_base = base

        self._dma = getattr(self.overlay, dma_name, None)
        if self._dma is None or not hasattr(self._dma, "sendchannel"):
            names = ", ".join(sorted(getattr(self.overlay, "ip_dict", {}))) or "（空）"
            raise PqcAccelError(
                f"overlay 里找不到可用的 AXI-DMA 实例 {dma_name!r}；"
                f"现有 IP：{names}。名字取决于 block design，"
                f"以 create_project.tcl 里的实例名为准。")

        # 按长度缓存缓冲。每条命令重新 allocate 一次会不断向 CMA 申请与归还，
        # 长时间跑下来容易碰上碎片；长度种类只有两三种，缓存起来最省事。
        self._in_bufs: dict[int, object] = {}
        self._out_bufs: dict[int, object] = {}

        self.last_poll_count = 0        # 上一条命令轮询了多少次 STATUS

    # ---- 地址解析 ----

    def _resolve_accel(self, accel_name: str):
        """从 overlay 的 IP 字典里取加速器的基址

        先按名字找，找不到再按物理地址反查 —— 与 accel_zynq.c 里 UIO 的做法
        同一个理由：名字取决于 block design 怎么写，地址是硬件事实。
        两条路都没结果时退回头文件里的常量，并把这件事说出来。
        """
        ip_dict = getattr(self.overlay, "ip_dict", None) or {}
        entry = ip_dict.get(accel_name)
        if entry is None:
            for info in ip_dict.values():
                if isinstance(info, dict) and info.get("phys_addr") == ACCEL_BASE:
                    entry = info
                    break
        if entry is None:
            print(f"[警告] overlay 里没找到 {accel_name!r}，也没有基址为 "
                  f"0x{ACCEL_BASE:08X} 的 IP；按常量 0x{ACCEL_BASE:08X} 映射。"
                  f"若 .hwh 与 .bit 不配套，读 VERSION 会得到无意义的值。")
            return ACCEL_BASE, ACCEL_SPAN
        base = int(entry.get("phys_addr", ACCEL_BASE))
        span = int(entry.get("addr_range", ACCEL_SPAN))
        if base != ACCEL_BASE:
            print(f"[警告] .hwh 给出的加速器基址 0x{base:08X} 与 "
                  f"include/pqc_accel_zynq.h 里的 0x{ACCEL_BASE:08X} 不一致。"
                  f"以 .hwh 为准继续，但 C 侧那条路径会读错地址。")
        return base, span

    # ---- 寄存器 ----

    @property
    def version(self) -> int:
        return self._regs.read(REG_VERSION)

    @property
    def status(self) -> int:
        return self._regs.read(REG_STATUS)

    @property
    def errcode(self) -> int:
        return self._regs.read(REG_ERRCODE)

    @property
    def out_len(self) -> int:
        return self._regs.read(REG_OUT_LEN)

    def reset(self) -> None:
        """软复位加速器，并把两个 DMA 通道拉回停止再启动

        加速器的 SOFT_RESET 只管 PL 内部的数据通路与状态位，DMA 是另一个 IP，
        它身上可能还挂着一次没做完的传输，必须一起收拾干净。
        """
        self._regs.write(REG_CTRL, CTRL_SOFT_RESET)
        self._restart_channel(self._dma.recvchannel)
        self._restart_channel(self._dma.sendchannel)

    @staticmethod
    def _restart_channel(channel) -> None:
        """停掉再启动一个 DMA 通道；失败不往上抛

        这个函数只在收尾路径上被调用（命令出错、复位）。收尾失败本身没有意义，
        把它抛出去只会盖掉真正的错误原因。
        """
        try:
            channel.stop()
            channel.start()
        except Exception as exc:            # 收尾动作，任何失败都只记录
            print(f"[警告] 重启 DMA 通道失败：{exc}")

    # ---- 等待 ----

    def _poll_done(self) -> int:
        """轮询 STATUS 直到 DONE 置位，返回读到的 STATUS

        DONE 是锁存的电平而不是脉冲，所以轮询的间隔无所谓，不会漏掉。
        """
        deadline = time.monotonic() + self._timeout
        polls = 0
        while True:
            st = self._regs.read(REG_STATUS)
            polls += 1
            if st & ST_DONE:
                self.last_poll_count = polls
                return st
            if time.monotonic() > deadline:
                self.last_poll_count = polls
                raise PqcAccelTimeout(
                    f"{self._timeout}s 内没等到 STATUS.DONE（轮询 {polls} 次，"
                    f"最后读到 STATUS=0x{st:08X}）。100 MHz 下最长的命令约 13 µs，"
                    f"超时说明命令根本没跑起来，不是还在算。")

    def _wait_channel(self, channel, what: str) -> None:
        """等一个 DMA 通道空闲，带时限

        PYNQ 自己的 wait() 也是轮询通道的 idle 位，这里用同一个判据，只是加上
        时限：流握手不通时 wait() 会一直转，整个脚本挂死，板上什么信息都拿不到。
        判据成立后仍然调一次 wait()，让 PYNQ 更新它自己的内部状态并检查错误位。
        """
        deadline = time.monotonic() + self._timeout
        while not channel.idle:
            if time.monotonic() > deadline:
                raise PqcAccelTimeout(
                    f"{self._timeout}s 内 {what} 通道没有空闲。"
                    f"输入方向超时说明加速器没有收数据（TREADY 一直为低），"
                    f"输出方向超时说明结果没有推出来。")
        channel.wait()

    # ---- 缓冲 ----

    def _in_buf(self, nbytes: int):
        buf = self._in_bufs.get(nbytes)
        if buf is None:
            buf = self._allocate(shape=(nbytes,), dtype=self._np.uint8)
            self._in_bufs[nbytes] = buf
        return buf

    def _out_buf(self, nbytes: int):
        buf = self._out_bufs.get(nbytes)
        if buf is None:
            buf = self._allocate(shape=(nbytes,), dtype=self._np.uint8)
            self._out_bufs[nbytes] = buf
        return buf

    # ---- 命令 ----

    def run(self, mode: int, payload: bytes, out_max: int = MAX_XFER) -> bytes:
        """走完整条命令时序，返回 OUT_LEN 个字节

        时序照 docs/register-map.md：
            武装接收通道 → 送入输入包 → 写 MODE → 写 IN_LEN → 写 CTRL=START
            → 轮询 STATUS.DONE → 有 ERR 则读 ERRCODE → 否则读 OUT_LEN 并取回

        接收通道必须在 START 之前武装好。加速器算完就把结果往外推，接收方还没
        就绪的话那几拍会因为 TREADY 为低而堵在 PL 里；这与 accel_zynq.c 里
        "先武装 S2MM，再启动 MM2S"是同一个理由。

        输入包整包进到 PL 之后才能写 START：加速器是"包收齐、写 START、再开算"
        的语义，START 早于数据到齐会算到上一次的残留。
        """
        n = len(payload)
        if n == 0 or n > MAX_XFER:
            raise ValueError(f"输入长度必须在 1..{MAX_XFER} 字节之间，收到 {n}")
        if not 0 < out_max <= MAX_XFER:
            raise ValueError(f"out_max 必须在 1..{MAX_XFER} 之间，收到 {out_max}")

        in_buf = self._in_buf(n)
        out_buf = self._out_buf(out_max)
        in_buf[:] = self._np.frombuffer(payload, dtype=self._np.uint8)
        # 到这里不需要 in_buf.flush()：allocate 给的是 dma-coherent 缓冲。

        recv = self._dma.recvchannel
        send = self._dma.sendchannel

        recv.transfer(out_buf)
        send.transfer(in_buf)
        self._wait_channel(send, "输入（MM2S）")

        self._regs.write(REG_MODE, int(mode))
        self._regs.write(REG_IN_LEN, n)
        self._regs.write(REG_CTRL, CTRL_START)

        try:
            st = self._poll_done()
        except PqcAccelTimeout:
            self._restart_channel(recv)
            raise

        if st & ST_ERR:
            code = self._regs.read(REG_ERRCODE)
            # 出错的命令不产生输出流，接收通道会一直挂在武装状态，必须收掉，
            # 否则下一条命令的 transfer 会落在一个仍在运行的通道上。
            self._restart_channel(recv)
            raise PqcAccelCommandError(
                f"命令失败：MODE={mode} IN_LEN={n} STATUS=0x{st:08X} "
                f"ERRCODE={code}", st, code)

        olen = self._regs.read(REG_OUT_LEN)
        if olen == 0 or olen > out_max:
            self._restart_channel(recv)
            raise PqcAccelError(
                f"OUT_LEN={olen} 不在 1..{out_max} 之间。"
                f"寄存器读到了不合理的值，先怀疑 .hwh 与 .bit 不配套。")

        self._wait_channel(recv, "输出（S2MM）")

        # 与 DMA 实际收回的字节数交叉核对。两者不一致说明搬运出了问题，
        # 此时缓冲里可能是上一次的残留，不能当成结果用。
        transferred = getattr(recv, "transferred", None)
        if transferred is not None and int(transferred) != olen:
            raise PqcAccelError(
                f"OUT_LEN={olen} 与 DMA 实际收回的 {int(transferred)} 字节不一致，"
                f"结果不可信。")

        # 同样不需要 out_buf.invalidate()：coherent 缓冲上 CPU 直接读到新数据。
        return bytes(memoryview(out_buf)[:olen])

    # ---- 对外的两个操作 ----

    def keccak_f1600(self, state_bytes: bytes) -> bytes:
        """对 200 字节的 Keccak 状态做一次 Keccak-f[1600] 置换

        状态就是 25 个 64 位 lane 的小端字节串，与 FIPS 202 的字节序一致。
        """
        if len(state_bytes) != KECCAK_LEN:
            raise ValueError(
                f"Keccak-f[1600] 的输入必须是 {KECCAK_LEN} 字节，"
                f"收到 {len(state_bytes)} 字节")
        out = self.run(MODE_KECCAK_F1600, bytes(state_bytes), KECCAK_LEN)
        if len(out) != KECCAK_LEN:
            raise PqcAccelError(f"Keccak 输出应为 {KECCAK_LEN} 字节，收到 {len(out)}")
        return out

    def ntt(self, coeffs, inverse: bool = False) -> list[int]:
        """256 点 NTT。inverse=True 时做逆变换。

        输入输出都是 16 位有符号系数的列表。约定与 hardware/model/ref_model.py
        的 ntt()/invntt() 完全一致，包括逆变换末尾那次 f 缩放 —— 因此
        ntt(ntt(x), inverse=True) 得到的是 x·2^16 mod q，而不是 x。
        """
        payload = pack_coeffs(coeffs)
        mode = MODE_NTT_INV if inverse else MODE_NTT_FWD
        out = self.run(mode, payload, NTT_LEN)
        return unpack_coeffs(out)

    # ---- 生命周期 ----

    def close(self) -> None:
        """归还 CMA 缓冲。不归还的话进程退出前这些物理连续内存一直占着。"""
        for bufs in (self._in_bufs, self._out_bufs):
            for buf in bufs.values():
                try:
                    buf.freebuffer()
                except Exception as exc:    # 归还失败只记录，不影响调用方
                    print(f"[警告] 归还 DMA 缓冲失败：{exc}")
            bufs.clear()

    def __enter__(self) -> "PqcAccel":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()
