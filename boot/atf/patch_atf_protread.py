#!/usr/bin/env python3
"""给 BL31 加一个**只读**的保护单元读口 —— 先把 XPPU/XMPU 的实配读出来

在构建机上跑：python3 patch_atf_protread.py [atf 源码根目录]
本脚本是 patch_atf_secread.py 的续集，两个可以先后打（互不冲突）。

============================================================================
【为什么必须先读、不能直接配】
============================================================================
"把 XMPU/XPPU 配上"这句话里藏着一个没人回答过的问题：**0x8000_0000 这段
PL aperture 到底归哪个保护单元管。** ZynqMP 上 XPPU 管 LPD 从机、XMPU_FPD
管 FPD 从机、XMPU_DDR 管 DDR、XMPU_OCM 管 OCM，而本设计的 PL 挂在
M_AXI_HPM0_LPD（maxigp2）上 —— 这条路上到底有没有保护单元、FSBL 与 PMUFW
已经替我们配了什么，都只能实测。

猜错的代价是具体的：配到一个根本不在路径上的单元，会得到一个"配了但没用"
的结果，而它看起来和"配对了"一模一样 —— 因为两者都不报错。

所以第一步只加**读**，一个字节都不写。

============================================================================
【只读也不是没有风险：EL3 上的取数错误没有人接得住】
============================================================================
在 EL3 读一个未实现的地址会产生同步外部中止，而 BL31 里没有处理它的东西 ——
发 SMC 的那个核当场卡死在异常里，其余核照常跑 Linux（"半死不活"的形状，
上一轮已经踩过一次，见 patch_atf_plmap.py 的文件头）。

两条约束因此是硬的：
  · **地址白名单**只放行四个保护单元的寄存器窗口，别的一律拒；
  · 普通世界那一侧**分批读、每批落盘**（见 board/kmod/protdump.c），
    卡住时能定位到具体是哪一个偏移，而不是丢掉整轮结果。

============================================================================
【页表：0xFD00_0000 那一段原本没有映射】
============================================================================
zynqmp_def.h 里 DEVICE0 = 0xFF00_0000 + 0xE0_0000，覆盖 XPPU（0xFF98_0000）
与 XMPU_OCM（0xFFA7_0000）；DEVICE1 = 0xF900_0000（GIC）。
**XMPU_DDR0..5（0xFD00_0000）与 XMPU_FPD（0xFD5D_0000）都不在里面。**
不补映射就去读，结果不是"读不到"，是 EL3 翻译错误、核卡死 —— 与上一轮
漏映射 PL 窗口时一模一样的坑。
"""
import sys

ATF = sys.argv[1] if len(sys.argv) > 1 else '/home/build/wdt_patch/atf'
SETUP = ATF + '/plat/xilinx/zynqmp/bl31_zynqmp_setup.c'
SIP = ATF + '/plat/xilinx/zynqmp/sip_svc_setup.c'
PLATDEF = ATF + '/plat/xilinx/zynqmp/include/platform_def.h'

# ---- 页表：补 FPD 那一段 ----
MAP_ANCHOR = """		MAP_REGION_FLAT(0x80000000U, 0x200000U,
				MT_DEVICE | MT_RW | MT_SECURE),"""

MAP_NEW = MAP_ANCHOR + """
		/*
		 * FPD 的保护单元寄存器：XMPU_DDR0..5（0xFD00_0000 起，每个
		 * 64 KB）与 XMPU_FPD（0xFD5D_0000）。DEVICE0/DEVICE1 都不覆盖
		 * 这一段 —— 不映射就去读，EL3 会翻译错误、发 SMC 的核卡死。
		 * 6 MB 一次覆盖到 0xFD5F_FFFF。
		 */
		MAP_REGION_FLAT(0xFD000000U, 0x600000U,
				MT_DEVICE | MT_RW | MT_SECURE),"""

# ---- SiP ----
DEFS_ANCHOR = '#define ZYNQMP_SIP_SVC_PL_SECREAD\t0x8200ff10'

DEFS_NEW = DEFS_ANCHOR + '''

/*
 * 自定义：在 EL3（安全世界）读一个**保护单元**寄存器。只读，没有写的对应物。
 *
 *   x1 = 物理地址（4 字节对齐，必须落在下面四个窗口之一）
 *   返回 x0 = 0 成功 / ~0 参数不合法；x1 = 读到的值
 *
 * 白名单必须窄：这是"把 XPPU/XMPU 的实配看清楚"用的口，不是一个
 * "EL3 读任意地址"的 oracle。窗口之外一律拒，拒的成本是零，
 * 而放宽一寸就等于把整个安全世界的地址空间交出去。
 */
#define ZYNQMP_SIP_SVC_PROT_READ\t0x8200ff11

/* XPPU 本体 + 400 条 aperture 许可表（0xFF98_1000 起） */
#define PROT_XPPU_LO\t\t\t0xFF980000ULL
#define PROT_XPPU_HI\t\t\t0xFF98FFFCULL
/* XMPU_DDR0..5，每个 64 KB */
#define PROT_XMPU_DDR_LO\t\t0xFD000000ULL
#define PROT_XMPU_DDR_HI\t\t0xFD05FFFCULL
/* XMPU_FPD */
#define PROT_XMPU_FPD_LO\t\t0xFD5D0000ULL
#define PROT_XMPU_FPD_HI\t\t0xFD5DFFFCULL
/* XMPU_OCM */
#define PROT_XMPU_OCM_LO\t\t0xFFA70000ULL
#define PROT_XMPU_OCM_HI\t\t0xFFA7FFFCULL'''

CASE_ANCHOR = '''	case ZYNQMP_SIP_SVC_PL_SECREAD: {'''

CASE_NEW = '''	case ZYNQMP_SIP_SVC_PROT_READ: {
		uint64_t a = (uint64_t)x1;
		int ok = 0;

		if ((a & 3ULL) == 0ULL) {
			if (a >= PROT_XPPU_LO && a <= PROT_XPPU_HI)
				ok = 1;
			else if (a >= PROT_XMPU_DDR_LO && a <= PROT_XMPU_DDR_HI)
				ok = 1;
			else if (a >= PROT_XMPU_FPD_LO && a <= PROT_XMPU_FPD_HI)
				ok = 1;
			else if (a >= PROT_XMPU_OCM_LO && a <= PROT_XMPU_OCM_HI)
				ok = 1;
		}
		if (ok == 0) {
			SMC_RET2(handle, (uint64_t)~0ULL, (uint64_t)0);
		}
		SMC_RET2(handle, (uint64_t)0,
			 (uint64_t)mmio_read_32((uintptr_t)a));
	}

	case ZYNQMP_SIP_SVC_PL_SECREAD: {'''


def patch(path, old, new, done_marker, what):
    s = open(path, encoding='utf-8').read()
    if done_marker in s:
        print(f'{what}：已经打过，跳过')
        return 0
    if old not in s:
        print(f'错误：{what} 找不到锚点')
        return 1
    open(path, 'w', encoding='utf-8').write(s.replace(old, new, 1))
    print(f'{what}：已应用')
    return 0


def main():
    rc = 0
    rc |= patch(SETUP, MAP_ANCHOR, MAP_NEW, '0xFD000000U, 0x600000U',
                'bl_regions 补 FPD 保护单元映射')
    rc |= patch(SIP, DEFS_ANCHOR, DEFS_NEW, 'ZYNQMP_SIP_SVC_PROT_READ',
                'SiP 定义')
    rc |= patch(SIP, CASE_ANCHOR, CASE_NEW, 'case ZYNQMP_SIP_SVC_PROT_READ',
                'SiP 分支')

    # MAX_MMAP_REGIONS：原值 9，bl_regions 已经 5 条 + plat 的 3 条 = 8，
    # 再加这一条正好顶到 9。顶满没有余量，下次再加一条就静默截断 ——
    # 而截断的表现是"读某个地址时核卡死"，查起来极贵。直接放宽。
    s = open(PLATDEF, encoding='utf-8').read()
    if '#define MAX_MMAP_REGIONS\t\t12' in s:
        print('MAX_MMAP_REGIONS：已经放宽过，跳过')
    elif '#define MAX_MMAP_REGIONS\t\t9' not in s:
        print('错误：找不到 MAX_MMAP_REGIONS 的定义')
        rc |= 1
    else:
        s = s.replace('#define MAX_MMAP_REGIONS\t\t9',
                      '#define MAX_MMAP_REGIONS\t\t12', 1)
        open(PLATDEF, 'w', encoding='utf-8').write(s)
        print('MAX_MMAP_REGIONS：9 → 12')
    return rc


if __name__ == '__main__':
    sys.exit(main())
