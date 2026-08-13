#!/usr/bin/env python3
"""给 ATF 的 SiP 处理加一个「EL3 读 PL 地址」的调用 —— 边界证明的另一半

在构建机上跑：python3 patch_atf_secread.py [atf 源码根目录]

为什么需要它：PL 里的 axi4lite_firewall 在 SECURE_ONLY=1 时要求
AxPROT[1]==0，否则回 DECERR。板上已经证明了一半 —— Linux（普通世界，
AxPROT[1] 恒为 1）读金丝雀必被 DECERR。另一半"安全世界能读"没法从普通
世界证：它根本发不出安全事务。

更轻的路都试过、都不成立：
 · PM_MMIO_READ 是把请求转给 PMU 执行的，而 PMUFW 的地址白名单不放行 PL 段
   （板上实测三个 PL 地址全返回 2002 = XST_PM_NO_ACCESS，而且对
   SECURE_ONLY=1 和 =0 一视同仁 —— 拒的是白名单不是我的门控，
   所以那条路什么都证明不了）；
 · ATF 里没有任何现成的"EL3 自己做 mmio"的 SiP。

BL31 跑在 EL3、SCR_EL3.NS=0，它的数据访问就是 AxPROT[1]=0 的安全事务。
"""
import re
import sys

ATF = sys.argv[1] if len(sys.argv) > 1 else '/home/build/wdt_patch/atf'
SRC = ATF + '/plat/xilinx/zynqmp/sip_svc_setup.c'

DEFS = '''#define ZYNQMP_SIP_SVC_VERSION\t\t0x8200ff03

/*
 * 自定义：在 EL3（安全世界）读一个 PL 地址。
 *
 * 两条自我约束：
 *  · **只放行 PL 窗口**。这是证明用的口子，不是通用的"EL3 读任意地址"
 *    oracle —— 别把它做成能读遍安全世界的东西。
 *  · **按需调用，不在启动时读**。金丝雀若因门控写反而回 DECERR，
 *    EL3 上是同步外部中止 → panic → 板子起不来。做成按需的，就能先确认
 *    板子起来、先用 SECURE_ONLY=0 的核验通路，再去碰金丝雀。
 *
 *   x1 = 物理地址（4 字节对齐、落在 PL 窗口内）
 *   返回 x0 = 0 成功 / ~0 参数不合法；x1 = 读到的值
 */
#define ZYNQMP_SIP_SVC_PL_SECREAD\t0x8200ff10
#define PL_WINDOW_LO\t\t\t0x80000000ULL
#define PL_WINDOW_HI\t\t\t0x8FFFFFFCULL'''

CASE_OLD = '''	case ZYNQMP_SIP_SVC_VERSION:
		SMC_RET2(handle, SIP_SVC_VERSION_MAJOR, SIP_SVC_VERSION_MINOR);
'''

CASE_NEW = '''	case ZYNQMP_SIP_SVC_VERSION:
		SMC_RET2(handle, SIP_SVC_VERSION_MAJOR, SIP_SVC_VERSION_MINOR);

	case ZYNQMP_SIP_SVC_PL_SECREAD: {
		uint64_t addr = (uint64_t)x1;

		if (addr < PL_WINDOW_LO || addr > PL_WINDOW_HI ||
		    (addr & 3ULL) != 0ULL) {
			SMC_RET2(handle, (uint64_t)~0ULL, (uint64_t)0);
		}

		/* EL3 + SCR_EL3.NS=0 → 这一笔就是 AxPROT[1]=0 的安全读 */
		SMC_RET2(handle, (uint64_t)0,
			 (uint64_t)mmio_read_32((uintptr_t)addr));
	}
'''


def main():
    s = open(SRC, encoding='utf-8').read()
    if 'ZYNQMP_SIP_SVC_PL_SECREAD' in s:
        print('已经打过补丁，跳过')
        return 0

    if '#include <lib/mmio.h>' not in s:
        s = s.replace('#include <common/runtime_svc.h>',
                      '#include <common/runtime_svc.h>\n#include <lib/mmio.h>', 1)

    old_def = '#define ZYNQMP_SIP_SVC_VERSION\t\t0x8200ff03'
    if old_def not in s:
        print('错误：找不到 ZYNQMP_SIP_SVC_VERSION 的定义')
        return 1
    s = s.replace(old_def, DEFS, 1)

    if CASE_OLD not in s:
        print('错误：找不到 ZYNQMP_SIP_SVC_VERSION 的 case 分支')
        return 1
    s = s.replace(CASE_OLD, CASE_NEW, 1)

    open(SRC, 'w', encoding='utf-8').write(s)
    print('补丁已应用到 ' + SRC)
    return 0


if __name__ == '__main__':
    sys.exit(main())
