#!/usr/bin/env python3
"""给 BL31 加一个**只读**的保护单元读口 —— 先把 XPPU/XMPU 的实配读出来

用法（构建机上）：
    python3 patch_atf_protread.py [atf 源码根目录]              # A1：最小改动
    python3 patch_atf_protread.py [atf 源码根目录] --with-fpd    # A2：再加 FPD 那半

本脚本**幂等**：每次先把自己上一次插入的内容整段删掉，再按当前档位重插。
所以 A1 与 A2 之间可以来回切，不会叠加出半新半旧的树。

============================================================================
【为什么必须先读、不能直接配】
============================================================================
"把 XMPU/XPPU 配上"这句话里藏着一个没人回答过的问题：**0x8000_0000 这段
PL aperture 到底归哪个保护单元管。** ZynqMP 上 XPPU 管 LPD 从机、XMPU_FPD
管 FPD 从机、XMPU_DDR 管 DDR、XMPU_OCM 管 OCM，而本设计的 PL 挂在
M_AXI_HPM0_LPD（maxigp2）上 —— 这条路上到底有没有保护单元、FSBL 与 PMUFW
已经替我们配了什么，只能实测。

猜错的代价很具体：配到一个根本不在路径上的单元，会得到一个"配了但没用"
的结果，而它和"配对了"长得一模一样 —— 两者都不报错。所以第一步只加**读**。

============================================================================
【A1 / A2 分两档 —— 一次断电换来的】
============================================================================
第一版一次动了三处：SiP 分支、`0xFD00_0000` 起 6 MB 的新页表映射、
`MAX_MMAP_REGIONS` 9→12。**板子起不来，只能断电。** 而
build-bl31-secread.sh 的文件头明明写着"只加一个 SiP 分支，其余逐字相同，
这样万一起不来变量只有一个"—— 自己写的纪律自己违反了。

关键在于这三处里有两处**根本不必要**：

    XPPU      0xFF98_0000  ┐
    XMPU_OCM  0xFFA7_0000  ┘ 都在 DEVICE0（0xFF00_0000 + 14 MB）里，
                             页表本来就有，一个字都不用改

    XMPU_DDR  0xFD00_0000  ┐ 这两个才需要新映射
    XMPU_FPD  0xFD5D_0000  ┘

而**主要问题（PL aperture 归谁管）的答案在 XPPU 的 400 条 aperture 许可表里**
—— 也就是说 A1 就够了。为了两个次要目标给主要目标平白加两个变量，
是这一轮唯一真正的错误。

于是：
  A1（默认）  只加 SiP 分支，白名单只有 XPPU + XMPU_OCM。**页表一个字不动。**
              与已知能启动的 BOOT0004 相比，变量只有一个。
  A2（--with-fpd）  确认 A1 能起来之后再加：DDR/FPD 两个窗口 + 那段映射
              + MAX_MMAP_REGIONS + MAX_XLAT_TABLES 一起放宽。

============================================================================
【只读也不是没有风险：EL3 上的取数错误没有人接得住】
============================================================================
在 EL3 读一个未实现的地址会产生同步外部中止，而 BL31 里没有处理它的东西 ——
发 SMC 的那个核当场卡死在异常里，其余核照常跑 Linux（"半死不活"的形状，
见 patch_atf_plmap.py 的文件头）。

两条约束因此是硬的：
  · **地址白名单**只放行保护单元的寄存器窗口，别的一律拒；
  · 普通世界那一侧**分组读、每组落盘**（见 board/kmod/protdump.c），
    卡住时能定位到具体是哪一段，而不是丢掉整轮结果。
"""
import re
import sys

ATF = '/home/build/wdt_patch/atf'
WITH_FPD = False
for a in sys.argv[1:]:
    if a == '--with-fpd':
        WITH_FPD = True
    else:
        ATF = a

SETUP = ATF + '/plat/xilinx/zynqmp/bl31_zynqmp_setup.c'
SIP = ATF + '/plat/xilinx/zynqmp/sip_svc_setup.c'
PLATDEF = ATF + '/plat/xilinx/zynqmp/include/platform_def.h'

BEG = '\t/* >>> pqchsm protread BEGIN */\n'
END = '\t/* <<< pqchsm protread END */\n'

# ---- 页表：只有 A2 才需要 ----
MAP_ANCHOR = """		MAP_REGION_FLAT(0x80000000U, 0x200000U,
				MT_DEVICE | MT_RW | MT_SECURE),
"""

MAP_BLOCK = BEG + """		/*
		 * FPD 的保护单元寄存器：XMPU_DDR0..5（0xFD00_0000 起，每个
		 * 64 KB）与 XMPU_FPD（0xFD5D_0000）。DEVICE0/DEVICE1 都不覆盖
		 * 这一段 —— 不映射就去读，EL3 会翻译错误、发 SMC 的核卡死。
		 * 6 MB 一次覆盖到 0xFD5F_FFFF，且 2 MB 对齐、长度是 2 MB 的整数倍，
		 * 所以只用块映射、不额外要 L3 表。
		 */
		MAP_REGION_FLAT(0xFD000000U, 0x600000U,
				MT_DEVICE | MT_RW | MT_SECURE),
""" + END

# ---- SiP 定义 ----
DEFS_ANCHOR = '#define ZYNQMP_SIP_SVC_PL_SECREAD\t0x8200ff10\n'

DEFS_COMMON = '''
/*
 * 自定义：在 EL3（安全世界）读一个**保护单元**寄存器。只读，没有写的对应物。
 *
 *   x1 = 物理地址（4 字节对齐，必须落在下面的窗口之一）
 *   返回 x0 = 0 成功 / ~0 参数不合法；x1 = 读到的值
 *
 * 白名单必须窄：这是"把 XPPU/XMPU 的实配看清楚"用的口，不是一个
 * "EL3 读任意地址"的 oracle。窗口之外一律拒，拒的成本是零，
 * 而放宽一寸就等于把整个安全世界的地址空间交出去。
 */
#define ZYNQMP_SIP_SVC_PROT_READ\t0x8200ff11

/* XPPU 本体 + 400 条 aperture 许可表（0xFF98_1000 起）。在 DEVICE0 里。 */
#define PROT_XPPU_LO\t\t\t0xFF980000ULL
#define PROT_XPPU_HI\t\t\t0xFF98FFFCULL
/* XMPU_OCM。也在 DEVICE0 里。 */
#define PROT_XMPU_OCM_LO\t\t0xFFA70000ULL
#define PROT_XMPU_OCM_HI\t\t0xFFA7FFFCULL
'''

DEFS_FPD = '''/* XMPU_DDR0..5，每个 64 KB。需要 A2 那段新映射。 */
#define PROT_XMPU_DDR_LO\t\t0xFD000000ULL
#define PROT_XMPU_DDR_HI\t\t0xFD05FFFCULL
/* XMPU_FPD。同上。 */
#define PROT_XMPU_FPD_LO\t\t0xFD5D0000ULL
#define PROT_XMPU_FPD_HI\t\t0xFD5DFFFCULL
'''

# ---- SiP 分支 ----
CASE_ANCHOR = '\tcase ZYNQMP_SIP_SVC_PL_SECREAD: {\n'

CASE_HEAD = '''	case ZYNQMP_SIP_SVC_PROT_READ: {
		uint64_t a = (uint64_t)x1;
		int ok = 0;

		if ((a & 3ULL) == 0ULL) {
			if (a >= PROT_XPPU_LO && a <= PROT_XPPU_HI)
				ok = 1;
			else if (a >= PROT_XMPU_OCM_LO && a <= PROT_XMPU_OCM_HI)
				ok = 1;
'''

CASE_FPD = '''			else if (a >= PROT_XMPU_DDR_LO && a <= PROT_XMPU_DDR_HI)
				ok = 1;
			else if (a >= PROT_XMPU_FPD_LO && a <= PROT_XMPU_FPD_HI)
				ok = 1;
'''

CASE_TAIL = '''		}
		if (ok == 0) {
			SMC_RET2(handle, (uint64_t)~0ULL, (uint64_t)0);
		}
		SMC_RET2(handle, (uint64_t)0,
			 (uint64_t)mmio_read_32((uintptr_t)a));
	}

'''


def strip_block(s):
    """把本脚本上一次插入的整段删掉（幂等的关键）"""
    while BEG in s and END in s:
        i = s.index(BEG)
        j = s.index(END) + len(END)
        s = s[:i] + s[j:]
    return s


def set_define(s, name, new_val):
    """改一个 #define 的值，**不依赖制表符个数**

    ⚠️ 第一版按两个制表符去匹配，而 MAX_XLAT_TABLES 那一行是三个 ——
    于是它静默地没被替换，脚本还照常打印"已放宽"。这类"看起来改了其实没改"
    最贵：MAX_XLAT_TABLES 不够时 BL31 在建页表时就断言失败，
    表现是**板子根本起不来**，而不是某个地址读不到。
    所以这里用正则，并且改完把实际行打印出来让人核对。
    """
    return re.sub(r'(#define\s+' + name + r'\s+)\d+', r'\g<1>' + str(new_val), s)


def main():
    # ---- 页表 ----
    s = strip_block(open(SETUP, encoding='utf-8').read())
    if WITH_FPD:
        if MAP_ANCHOR not in s:
            print('错误：找不到 PL 窗口映射那个锚点（先跑 patch_atf_plmap.py）')
            return 1
        s = s.replace(MAP_ANCHOR, MAP_ANCHOR + MAP_BLOCK, 1)
    open(SETUP, 'w', encoding='utf-8').write(s)
    print('bl_regions：' + ('已加 FPD 保护单元映射' if WITH_FPD else '未改动（A1 不需要）'))

    # ---- SiP ----
    s = strip_block(open(SIP, encoding='utf-8').read())
    if DEFS_ANCHOR not in s:
        print('错误：找不到 PL_SECREAD 的定义（先跑 patch_atf_secread.py）')
        return 1
    defs = BEG + DEFS_COMMON + (DEFS_FPD if WITH_FPD else '') + END
    s = s.replace(DEFS_ANCHOR, DEFS_ANCHOR + defs, 1)

    if CASE_ANCHOR not in s:
        print('错误：找不到 PL_SECREAD 的 case 分支')
        return 1
    case = BEG + CASE_HEAD + (CASE_FPD if WITH_FPD else '') + CASE_TAIL + END
    s = s.replace(CASE_ANCHOR, case + CASE_ANCHOR, 1)
    open(SIP, 'w', encoding='utf-8').write(s)
    print('SiP：已插入（窗口 = XPPU + XMPU_OCM'
          + ('  + XMPU_DDR + XMPU_FPD）' if WITH_FPD else '）'))

    # ---- 上限 ----
    # A1 一条映射都不加，所以两个上限都还原成原值；A2 才放宽。
    # ⚠️ MAX_XLAT_TABLES 与 MAX_MMAP_REGIONS 要**一起**放宽：
    #    前者不够时 BL31 在建页表时就断言失败，表现是**根本起不来**，
    #    而不是"读某个地址时出错" —— 第一版正是栽在没核算它上。
    s = open(PLATDEF, encoding='utf-8').read()
    if WITH_FPD:
        s = set_define(s, 'MAX_MMAP_REGIONS', 12)
        s = set_define(s, 'MAX_XLAT_TABLES', 10)
    else:
        s = set_define(s, 'MAX_MMAP_REGIONS', 9)
        s = set_define(s, 'MAX_XLAT_TABLES', 7)
    open(PLATDEF, 'w', encoding='utf-8').write(s)
    for nm in ('MAX_MMAP_REGIONS', 'MAX_XLAT_TABLES'):
        for line in s.splitlines():
            if line.startswith(f'#define {nm}'):
                print('  ' + line.strip())
    return 0


if __name__ == '__main__':
    sys.exit(main())
