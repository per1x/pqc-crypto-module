#!/usr/bin/env python3
"""给 BL31 的页表补上 PL 窗口的映射，并把 SiP 的地址检查收到与映射一致

在构建机上跑：python3 patch_atf_plmap.py [atf 源码根目录]

【为什么需要这一条 —— 一次实测换来的】
只加 SiP 分支是不够的。BL31 的 bl_regions[] 只映射它自己那几段，
plat_arm_get_mmap() 给的是 0xFF00_0000 那一带的外设 —— **PL 的
0x8000_0000 根本没在 EL3 的页表里**。

实测表现：SMC 发出去**再也不返回**。发 SMC 的那个核在 EL3 取数时翻译错误、
卡死在异常处理里，而其余三个核照常跑 Linux —— 所以板子看起来还活着，
最后是 plharness.sh 那条 480 秒看门狗把它重启的。这个"半死不活"的形状
很容易被误读成 SMC ABI 写错了，其实是页表缺一段。

【两处必须一致】
映射 2 MB（0x8000_0000–0x801F_FFFF，覆盖全部六个槽），SiP 的地址检查也收到
同一范围。不一致的话，给一个映射外但通过了检查的地址，照样在 EL3 翻译错误
卡死 —— 那就把一个"读寄存器的口子"做成了"挂板子的口子"。
"""
import sys

ATF = sys.argv[1] if len(sys.argv) > 1 else '/home/build/wdt_patch/atf'
SETUP = ATF + '/plat/xilinx/zynqmp/bl31_zynqmp_setup.c'
SIP = ATF + '/plat/xilinx/zynqmp/sip_svc_setup.c'

MAP_OLD = """		MAP_REGION_FLAT(BL_COHERENT_RAM_BASE,
				BL_COHERENT_RAM_END - BL_COHERENT_RAM_BASE,
				MT_DEVICE | MT_RW | MT_SECURE),
		{0}"""

MAP_NEW = """		MAP_REGION_FLAT(BL_COHERENT_RAM_BASE,
				BL_COHERENT_RAM_END - BL_COHERENT_RAM_BASE,
				MT_DEVICE | MT_RW | MT_SECURE),
		/*
		 * PL 的 AXI 窗口 —— 边界证明要在 EL3 上读这里（见
		 * sip_svc_setup.c 的 ZYNQMP_SIP_SVC_PL_SECREAD）。
		 * 不映射的话那笔读会在 EL3 翻译错误，发 SMC 的核卡死在
		 * 异常处理里、再也不返回（实测过，表现是板子半死不活：
		 * 其余核照常跑 Linux，最后被外部看门狗重启）。
		 * 只映 2 MB，正好覆盖六个槽；范围与 SiP 的地址检查保持一致。
		 */
		MAP_REGION_FLAT(0x80000000U, 0x200000U,
				MT_DEVICE | MT_RW | MT_SECURE),
		{0}"""


def main():
    s = open(SETUP, encoding='utf-8').read()
    if '0x80000000U, 0x200000U' in s:
        print('bl31_zynqmp_setup.c 已经打过，跳过')
    else:
        if MAP_OLD not in s:
            print('错误：找不到 bl_regions[] 里的锚点')
            return 1
        open(SETUP, 'w', encoding='utf-8').write(s.replace(MAP_OLD, MAP_NEW, 1))
        print('已给 bl_regions[] 加上 PL 窗口映射')

    t = open(SIP, encoding='utf-8').read()
    old = '#define PL_WINDOW_HI\t\t\t0x8FFFFFFCULL'
    new = ('/* 上界必须与 bl31_zynqmp_setup.c 里映射的那 2 MB 完全一致 ——\n'
           " * 检查放得比映射宽，就等于放行一个会在 EL3 翻译错误的地址。 */\n"
           '#define PL_WINDOW_HI\t\t\t0x801FFFFCULL')
    if '0x801FFFFCULL' in t:
        print('sip_svc_setup.c 的上界已经收过，跳过')
    elif old not in t:
        print('错误：找不到 PL_WINDOW_HI')
        return 1
    else:
        open(SIP, 'w', encoding='utf-8').write(t.replace(old, new, 1))
        print('已把 SiP 的地址上界收到 0x801FFFFC')
    return 0


if __name__ == '__main__':
    sys.exit(main())
