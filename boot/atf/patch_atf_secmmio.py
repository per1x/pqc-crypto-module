#!/usr/bin/env python3
"""把 BL31 的 SiP 收成一个**受限的安全 MMIO 读写服务**（送检口径的另一半）

在构建机上跑：python3 patch_atf_secmmio.py [atf 源码根目录]

本脚本**幂等**，而且它建立的是**最终状态**：无论树上先前打过 secread 还是
protread，跑完之后 SiP 里只剩这一个服务。

============================================================================
【它要证明什么】
============================================================================
先前那一版 bitstream 把四个功能核设成 SECURE_ONLY=0、另设一个 SECURE_ONLY=1
的金丝雀，于是"AxPROT 门控真的生效"是靠金丝雀证的。那个证明成立，但缺一块：
**被门控保护的只有金丝雀那个空壳，不是密码核本身。**

这一版把四个功能核全部设成 SECURE_ONLY=1，普通世界一个寄存器都摸不到，
整套 KAT 改由安全世界驱动 —— 每一笔核访问都经这个 SiP 从 EL3 发出。
于是命题变成完整的两条：

  · **正向**：SECURE_ONLY=1 的全功能核，由安全世界端到端跑完整套 KAT；
  · **反向**：同一批地址从普通世界直接读，全部 DECERR。

============================================================================
【白名单：比 xbar 的判据更窄，绝不更宽】
============================================================================
PL 里的 axi4lite_xbar 已经有一套译码判据（aperture / 槽号 / 槽内偏移高位 /
4 字节对齐）。**这里的白名单必须是它的子集。**两处策略各说各话，正是
axi4lite_xbar 文件头里写的那个坑 —— 改一处忘另一处，而且不报错。

所以这里在同样的四条之上再往窄收：每个槽只放行**该核实际存在的寄存器偏移**
（= 各核自己的防火墙窗口，见表上方的注释），并按槽分别给读/写权限：

    槽 0  0x8000_0000  trng_axi        0x00-0x3C   读 写
    槽 1  0x8001_0000  key_vault_axi   0x00-0x3C   读 写
    槽 2  0x8002_0000  sym_axi         0x00-0x7C   读 写
    槽 3  0x8003_0000  mlkem_axi       0x00-0x3C   读 写
    槽 4  0x8004_0000  金丝雀           0x00-0x3C   读 ——   （对照，写它没有用途）
    槽 5  0x8005_0000  fan_ctrl_axi    ——   ——     整核排除

风扇整核排除：KAT 用不着它，而它的 DRP 窗口被写坏会扰动 SYSMON 的温度采样。
最小权限不是姿态，是少一个能出事的地方。

============================================================================
【这个口**不能**变成"EL3 读写任意物理地址"】
============================================================================
这是本次扩展唯一真正的风险：为了证明边界而开的口子，本身不能比它证明的东西
更大。三条自我约束：

  · 边界与槽表全是**编译期常量**，没有任何一条总线路径能改动它
    （与防火墙策略用 parameter 而不是寄存器，是同一条理由）；
  · 判据函数**没有循环、没有指针算术**，一张表加四个比较，可以一眼审完；
  · **不做块传输**。整套 KAT 约 10 万次访问、每次一个 32 位字，
    按每次 ~2 µs 算不到 0.3 秒 —— 性能不构成把口子开大的理由。

顺带把**不再需要的口子删掉**：
  · PROT_READ（XPPU/XMPU 只读）—— XMPU 那条线已由 UG1085 结案，证据已归档；
  · PL_SECREAD（0x8000_0000–0x801F_FFFC 只读）—— 比新的读服务宽得多，
    而新服务已经覆盖它的全部用途（读金丝雀）。
安全世界的口子只应该留正在用的那些。

============================================================================
【EL3 上的取数错误没有人接得住 —— 调用方必须先确认 PL 在】
============================================================================
若 PL 没配置好就调这个 SiP，那笔访问会在 EL3 上出错，而 BL31 里没有处理它的
东西：发 SMC 的核当场卡死。所以闸门放在**普通世界**、而且**不碰 PL 总线**：
先读 /sys/class/fpga_manager/fpga0/state 确认已 programmed，再发第一笔 SMC
（见 board/kmod/secmmio.c）。兜底是 plharness 的 480 秒看门狗，
以及 multiboot 由 POR 清零 —— 断一次电就回黄金镜像。
"""
import re
import sys

ATF = sys.argv[1] if len(sys.argv) > 1 else '/home/build/wdt_patch/atf'
SETUP = ATF + '/plat/xilinx/zynqmp/bl31_zynqmp_setup.c'
SIP = ATF + '/plat/xilinx/zynqmp/sip_svc_setup.c'
PLATDEF = ATF + '/plat/xilinx/zynqmp/include/platform_def.h'

BEG = '/* >>> pqchsm secmmio BEGIN */'
END = '/* <<< pqchsm secmmio END */'

DEFS = BEG + '''
/*
 * 受限的安全 MMIO 服务：EL3 读/写 PL 里几个密码核的寄存器。
 *
 *   0x8200ff12  读：x1 = 物理地址          → x0 = 0/~0，x1 = 值
 *   0x8200ff13  写：x1 = 物理地址，x2 = 值 → x0 = 0/~0
 *
 * 白名单是 PL 内 axi4lite_xbar 译码判据的**子集**（见本文件对应的补丁脚本）。
 * 边界全是编译期常量；判据无循环、无指针算术。
 */
#define ZYNQMP_SIP_SVC_PL_RD\t\t0x8200ff12
#define ZYNQMP_SIP_SVC_PL_WR\t\t0x8200ff13

#define PL_BASE\t\t\t\t0x80000000ULL
#define PL_SLOT_SHIFT\t\t\t16
#define PL_NSLOT\t\t\t6

/*
 * 每槽的合法偏移上界（含）= **该核自己的窗口**。
 *
 * ⚠️ 第一版是照各核 localparam A_* 的最大索引手抄的，结果 sym_axi 抄成 0x30
 *    而它实际到 0x60 —— DOUT0..3 与 DIGEST0..7 那两块是用另一个表达式
 *    （f_araddr[6:2]）解码的，grep localparam 根本看不见。上板时 hsm_hwtest
 *    读 0x8002_0034 当场被拒。
 *
 *    失败方向是安全的（拒绝、报地址、什么都没坏），但根因是**手抄一份会漂移**。
 *    所以改成引用一个 RTL 里已经存在的策略：各核自己的防火墙窗口
 *    （axi4lite_firewall 的 ADDR_MASK；trng 是内联门控，按其 araddr[5:2] 解码）。
 *      key_vault / mlkem  ADDR_MASK=0xC0 → 0x00-0x3F
 *      sym                ADDR_MASK=0x80 → 0x00-0x7F
 *      trng               araddr[5:2]    → 0x00-0x3F
 *    这样只有一处定义，改 RTL 时不会有第二处忘了跟。
 */
static const uint32_t pl_off_max[PL_NSLOT] = {
\t0x3C,\t/* 0 trng_axi       araddr[5:2]        */
\t0x3C,\t/* 1 key_vault_axi  ADDR_MASK 0xC0     */
\t0x7C,\t/* 2 sym_axi        ADDR_MASK 0x80     */
\t0x3C,\t/* 3 mlkem_axi      ADDR_MASK 0xC0     */
\t0x3C,\t/* 4 金丝雀（只读，对照用）            */
\t0x00,\t/* 5 fan_ctrl_axi   整核排除           */
};
static const uint8_t pl_rd_ok[PL_NSLOT] = { 1, 1, 1, 1, 1, 0 };
static const uint8_t pl_wr_ok[PL_NSLOT] = { 1, 1, 1, 1, 0, 0 };

static int pl_permit(uint64_t a, int is_write)
{
\tuint64_t rel, off;
\tuint32_t slot;

\tif ((a & 3ULL) != 0ULL)
\t\treturn 0;
\tif (a < PL_BASE)
\t\treturn 0;
\trel = a - PL_BASE;
\tif (rel >= ((uint64_t)PL_NSLOT << PL_SLOT_SHIFT))
\t\treturn 0;
\tslot = (uint32_t)(rel >> PL_SLOT_SHIFT);
\toff = rel & 0xFFFFULL;
\t/* 这一条同时收掉了 addr[15:8] != 0：上界最大也只有 0x7C */
\tif (off > (uint64_t)pl_off_max[slot])
\t\treturn 0;
\treturn is_write ? (int)pl_wr_ok[slot] : (int)pl_rd_ok[slot];
}
''' + END

CASES = '\t' + BEG + '''
\tcase ZYNQMP_SIP_SVC_PL_RD:
\t\tif (pl_permit((uint64_t)x1, 0) == 0) {
\t\t\tSMC_RET2(handle, (uint64_t)~0ULL, (uint64_t)0);
\t\t}
\t\tSMC_RET2(handle, (uint64_t)0,
\t\t\t (uint64_t)mmio_read_32((uintptr_t)x1));

\tcase ZYNQMP_SIP_SVC_PL_WR:
\t\tif (pl_permit((uint64_t)x1, 1) == 0) {
\t\t\tSMC_RET2(handle, (uint64_t)~0ULL, (uint64_t)0);
\t\t}
\t\tmmio_write_32((uintptr_t)x1, (uint32_t)x2);
\t\tSMC_RET2(handle, (uint64_t)0, (uint64_t)0);
''' + '\t' + END


def strip_marked(s, tag):
    """删掉 /* >>> pqchsm <tag> BEGIN */ … END 之间的整段（含标记）"""
    b = '/* >>> pqchsm %s BEGIN */' % tag
    e = '/* <<< pqchsm %s END */' % tag
    while b in s and e in s:
        i = s.index(b)
        j = s.index(e) + len(e)
        # 连同标记所在行的前导空白与尾随换行一起去掉
        while i > 0 and s[i - 1] in ' \t':
            i -= 1
        if j < len(s) and s[j] == '\n':
            j += 1
        s = s[:i] + s[j:]
    return s


def strip_secread(s):
    """删掉 patch_atf_secread.py + patch_atf_plmap.py 装的 PL_SECREAD

    定义段：从 PL_SECREAD 那个 #define 往回走到最近的行首 /*，
            往后走到 PL_WINDOW_HI 那一行结束。
    分支段：从 case ZYNQMP_SIP_SVC_PL_SECREAD 到它的闭合大括号。
    两段都用锚点定位而不是抄一遍原文 —— 原文里有中文注释，
    抄错一个字就静默不匹配（这类"看起来改了其实没改"已经栽过一次）。
    """
    k = s.find('#define ZYNQMP_SIP_SVC_PL_SECREAD')
    if k >= 0:
        i = s.rfind('\n/*', 0, k)
        i = 0 if i < 0 else i + 1
        m = re.search(r'#define PL_WINDOW_HI[^\n]*\n', s[k:])
        j = k + m.end() if m else k
        s = s[:i] + s[j:]

    k = s.find('\tcase ZYNQMP_SIP_SVC_PL_SECREAD: {')
    if k >= 0:
        m = re.search(r'\n\t\}\n\n', s[k:])
        if m:
            s = s[:k] + s[k + m.end():]
    return s


def main():
    # ---- 页表：只保留 PL 窗口那一段，去掉 protread 的 FPD 段 ----
    s = open(SETUP, encoding='utf-8').read()
    s = strip_marked(s, 'protread')
    open(SETUP, 'w', encoding='utf-8').write(s)
    keep = s.count('0x80000000U, 0x200000U')
    print('bl_regions：FPD 段已去；PL 窗口映射保留 %d 处（必须为 1）' % keep)
    if keep != 1:
        print('错误：PL 窗口映射不见了 —— 先跑 patch_atf_plmap.py')
        return 1

    # ---- SiP ----
    s = open(SIP, encoding='utf-8').read()
    s = strip_marked(s, 'protread')
    s = strip_marked(s, 'secmmio')
    s = strip_secread(s)
    for tok in ('PROT_READ', 'PL_SECREAD', 'PL_WINDOW_HI'):
        if tok in s:
            print('错误：%s 没删干净' % tok)
            return 1

    anchor = '#define ZYNQMP_SIP_SVC_VERSION\t\t0x8200ff03\n'
    if anchor not in s:
        print('错误：找不到 VERSION 定义锚点')
        return 1
    s = s.replace(anchor, anchor + '\n' + DEFS + '\n', 1)

    canchor = '\tcase ZYNQMP_SIP_SVC_VERSION:\n\t\tSMC_RET2(handle, SIP_SVC_VERSION_MAJOR, SIP_SVC_VERSION_MINOR);\n'
    if canchor not in s:
        print('错误：找不到 VERSION 分支锚点')
        return 1
    s = s.replace(canchor, canchor + '\n' + CASES + '\n', 1)

    if '#include <lib/mmio.h>' not in s:
        s = s.replace('#include <common/runtime_svc.h>',
                      '#include <common/runtime_svc.h>\n#include <lib/mmio.h>', 1)
    open(SIP, 'w', encoding='utf-8').write(s)
    print('SiP：已装 PL_RD/PL_WR；PROT_READ 与 PL_SECREAD 已删除')

    # ---- 上限还原：不加映射就不该放宽 ----
    s = open(PLATDEF, encoding='utf-8').read()
    s = re.sub(r'(#define\s+MAX_MMAP_REGIONS\s+)\d+', r'\g<1>9', s)
    s = re.sub(r'(#define\s+MAX_XLAT_TABLES\s+)\d+', r'\g<1>7', s)
    open(PLATDEF, 'w', encoding='utf-8').write(s)
    for nm in ('MAX_MMAP_REGIONS', 'MAX_XLAT_TABLES'):
        for line in s.splitlines():
            if line.startswith('#define ' + nm):
                print('  ' + line.strip())
    return 0


if __name__ == '__main__':
    sys.exit(main())
