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
    槽 6  0x8006_0000  mldsa_axi       0x00-0x3C   读 写

另有一个**不在 PL 里**的只读窗口：

    ——    0xFFCA_0050-5C  设备 DNA（CSU）        读 ——   写一律拒绝

它在 CSU 里，EL1 直接读会挨总线错误。放它出来只为一件事：把密钥派生根从
"编译进去的常量"换成"绑到这块芯片"。**DNA 不是秘密**（有 JTAG 就能读），
所以它给的是防克隆，不是密钥的硬件保护 —— 详见下面 CSU_DNA_LO 处的长注释。

风扇整核排除：KAT 用不着它，而它的 DRP 窗口被写坏会扰动 SYSMON 的温度采样。
最小权限不是姿态，是少一个能出事的地方。

⚠️ **槽 6（ML-DSA）是后加的，加它的理由值得记。** 这张表原来只到槽 5，
而 ML-DSA 落在槽 6 —— 于是送检形态（SECURE_ONLY=1）下 daemon 经 /dev/secmmio
去读 0x8006_0000 必然被 EL3 拒掉，**与位流对不对无关**。表现极具误导性：
位流明明装好了、核也在，但任何以"读 mldsa VERSION"为判据的自检都必然失败，
于是 try_mldsa.sh 的自检判据被迫改成读风扇占空比（见那个脚本里的长注释），
而"ML-DSA 在送检形态下不可达"被当成了一条既成事实写进了好几处文档。
根因只是这张表少一行。**加从机就要同时加这一行**，否则新核在送检形态下
等于不存在。

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

/*
 * ---- 设备 DNA 只读窗口 ------------------------------------------------------
 * 0xFFCA0050..0xFFCA005C 在 CSU 里，**EL1 直接读会挨总线错误**（板上实测：
 * devmem 读这几个地址一律 "Bus error"，进程被 SIGBUS 打掉，板子本身没事 ——
 * 这也印证了本项目的老结论：读的拒绝接得住，写的拒绝是 SError、接不住）。
 *
 * 为什么要放它出来：密钥派生根（src/crypto/kdr.c）本来是一个编译进去的常量，
 * 换句话说把 keystore 整个拷到另一块板上照样能打开。绑到 DNA 之后就不能了。
 *
 * ⚠️ **DNA 不是秘密。** 谁有 JTAG 谁就能读到它（本项目就是这么读到的）。
 *    所以它给的是**设备绑定 / 防克隆**，不是"密钥受硬件保护"。OP-TEE 在这颗
 *    片子上的 HUK 也是 SHA-256(Device DNA)，性质完全一样 —— 不比它强，也不比
 *    它弱。文档里必须按这个口径写，别升级成"硬件密钥根"。
 *
 * ⚠️ 这些字**跨复位不变**是实测的（多次重启 + 一轮 BBRAM 烧写之间逐字相同）。
 *    但"它在不同芯片之间不同"**没有验证过** —— 手上只有一块板，无法证伪。
 *
 * 写一律拒绝：这个窗口只读。
 */
#define CSU_DNA_LO\t\t\t0xFFCA0050ULL
#define CSU_DNA_HI\t\t\t0xFFCA005CULL

/*
 * ---- 保护单元（XMPU / XPPU）只读窗口 ----------------------------------------
 * 这几段放开**只读**，理由只有一个：不放开就**无法观测**这块板此刻实际生效的
 * 内存保护是什么样。板上实测（board/src/xmpu_probe）：从 EL1-NS 读 XPPU 与
 * 六个 XMPU_DDR 的每一个寄存器，**无一例外都是总线错误** —— 保护单元自己的
 * 配置寄存器也被 XPPU 保护着。
 *
 * 于是"XMPU/XPPU 配了没有、配成什么样"这个问题在此之前只能靠读厂家的
 * psu_init 猜。而本项目吃过太多次"照文档抄一份会漂移"的亏（白名单表漏槽 6、
 * sym_axi 窗口抄成 0x30），所以宁可开一个只读口去读硬件。
 *
 * ⚠️ **只读，写一律拒绝。** 能写 XMPU/XPPU 等于能关掉全部内存保护 ——
 *    那比这个 SiP 已有的 PL 写口危险一个量级。这里绝不放写。
 *
 * ⚠️ 这确实是**攻击面的净增加**：普通世界从此能读出保护配置，等于给攻击者
 *    省了侦察。判断是"值得"：配置本身不是秘密（它由启动链写死，任何拿到同款
 *    板子的人都能在自己板上读出来），而看不见它的代价是一直在猜。
 *    docs/SECURITY 的"残余能力"表里要写上这一条。
 *
 * 覆盖：XPPU 控制块 + 许可表、XMPU_DDR0..5、XMPU_FPD、XMPU_OCM。
 */
#define XPPU_LO\t\t\t\t0xFF980000ULL
#define XPPU_HI\t\t\t\t0xFF981FFFULL
#define XMPU_DDR_LO\t\t\t0xFD000000ULL
#define XMPU_DDR_HI\t\t\t0xFD05FFFFULL
#define XMPU_FPD_LO\t\t\t0xFD5D0000ULL
#define XMPU_FPD_HI\t\t\t0xFD5D0FFFULL
#define XMPU_OCM_LO\t\t\t0xFFA70000ULL
#define XMPU_OCM_HI\t\t\t0xFFA70FFFULL

#define PL_BASE\t\t\t\t0x80000000ULL
#define PL_SLOT_SHIFT\t\t\t16
#define PL_NSLOT\t\t\t7

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
\t0x3C,\t/* 6 mldsa_axi      ADDR_MASK 0xC0     */
};
static const uint8_t pl_rd_ok[PL_NSLOT] = { 1, 1, 1, 1, 1, 0, 1 };
static const uint8_t pl_wr_ok[PL_NSLOT] = { 1, 1, 1, 1, 0, 0, 1 };

static int pl_permit(uint64_t a, int is_write)
{
\tuint64_t rel, off;
\tuint32_t slot;

\tif ((a & 3ULL) != 0ULL)
\t\treturn 0;
\t/* 设备 DNA：只读，且必须在 PL 区间判定之前 —— 它的地址远在 PL_BASE 之上，
\t * 落到下面的 rel 计算里会被当成越界槽直接拒掉。 */
\tif (a >= CSU_DNA_LO && a <= CSU_DNA_HI)
\t\treturn is_write ? 0 : 1;
\t/* 保护单元：只读。写一律拒绝 —— 能写它就能关掉全部内存保护。 */
\tif (is_write == 0) {
\t\tif ((a >= XPPU_LO && a <= XPPU_HI) ||
\t\t    (a >= XMPU_DDR_LO && a <= XMPU_DDR_HI) ||
\t\t    (a >= XMPU_FPD_LO && a <= XMPU_FPD_HI) ||
\t\t    (a >= XMPU_OCM_LO && a <= XMPU_OCM_HI))
\t\t\treturn 1;
\t}
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
    s = strip_marked(s, 'secmmio')
    keep = s.count('0x80000000U, 0x200000U')
    print('bl_regions：PL 窗口映射保留 %d 处（必须为 1）' % keep)
    if keep != 1:
        print('错误：PL 窗口映射不见了 —— 先跑 patch_atf_plmap.py')
        return 1

    # ---- 页表：FPD 里的保护单元 ----------------------------------------------
    # ⚠️ **这一段和 SiP 白名单是死死绑在一起的**：白名单放行了 0xFD00_0000 段
    #    的只读，而 EL3 的页表里默认**没有**这一段（平台的 DEVICE0 只覆盖
    #    0xFF00_0000-0xFFDF_FFFF）。少了它，那笔读会在 EL3 翻译错误 ——
    #    发 SMC 的核卡死在异常处理里再也不返回。上面 PL 窗口那段注释记着
    #    这块板上真栽过一次的表现："其余核照常跑 Linux，板子半死不活"。
    #
    #    所以：**改白名单里的地址段，就必须同时改这里。** 两处对不上不会报错，
    #    只会在第一次读到那个地址时把板子挂掉。
    #
    # 只读语义由白名单保证，页表这边仍标 MT_RW —— MT_RO 在 EL3 device 映射上
    # 不是所有 ATF 版本都受支持，而多一处"看着更安全实则没验过"的差异不值得。
    # ⚠️ **只映 1 MB，不是 6 MB。** 第一版写了 0x600000，BL31 直接死在
    #    xlat_tables_common.c:135 的 `separated_va && separated_pa` 断言上 ——
    #    平台自己的 plat_arm_mmap 里已经有一条
    #        CRF_APB  0xFD1A0000 + 0x600000   （到 0xFD79FFFF）
    #    我那 6 MB 压在它上面了。xlat 表不允许区域重叠，哪怕属性完全一样。
    #
    #    症状值得记：串口能看到 FSBL 横幅、能看到 BL31 的 VERBOSE 把每个
    #    region 逐条打出来（最后一条正是 0xfd000000-0xfd600000），然后
    #    ASSERT + BACKTRACE，**永远走不到 Starting kernel**。靠槽 6 + JTAG
    #    清 multiboot 救回来，没断电。
    #
    #    1 MB（0xFD000000-0xFD0FFFFF）已经覆盖 XMPU_DDR0..5（到 0xFD05FFFF）。
    #    XMPU_FPD 在 0xFD5D0000，**本来就落在 CRF_APB 那条里**，不用另外映。
    fpd = ('\t\t' + BEG + '\n'
           '\t\tMAP_REGION_FLAT(0xFD000000U, 0x100000U,\n'
           '\t\t\t\tMT_DEVICE | MT_RW | MT_SECURE),\n'
           '\t\t' + END + '\n')
    manchor = '\t\tMAP_REGION_FLAT(0x80000000U, 0x200000U,\n\t\t\t\tMT_DEVICE | MT_RW | MT_SECURE),\n'
    if manchor not in s:
        print('错误：找不到 PL 窗口那段映射，没法在它后面插 FPD 段')
        return 1
    s = s.replace(manchor, manchor + fpd, 1)
    open(SETUP, 'w', encoding='utf-8').write(s)
    print('bl_regions：已加 XMPU_DDR 映射 0xFD000000+0x100000（%d 处）'
          % s.count('0xFD000000U, 0x100000U'))

    # ---- XMPU_DDR 配置：**试过了，不生效，所以这里故意什么都不做** --------
    #
    # 别再往这里加"配一下 XMPU 保护 OP-TEE"的代码，除非先解决下面这个问题。
    #
    # 已经查实的现状（board/src/protunit_probe，经 EL3 只读窗口读出来的实配）：
    #   · XPPU_CTRL = 0            —— XPPU 整个是关的
    #   · XMPU_DDR0..5 CTRL = 0xb  —— 默认放行读写
    #   · 六个实例里**一个使能的区域都没有**
    # 反证不是推论，是读出来的：
    #   root@petalinux:~# devmem 0x60000000 32
    #   0xAA0003F3                     ← 一条 AArch64 指令
    # 那是 **OP-TEE 的安全世界代码，被非安全的 Linux 用户态直接读出来了**。
    #
    # 试过的修法（每一轮都真的上板启动过，不是编译过就算）：
    #   ① 六个实例 × R15，范围 0x6000_0000-0x6FFF_FFFF，cfg=0x0F
    #      → 六个实例都读回 0x0000000F，**区域确实在表里**；devmem 照样读得到。
    #   ② 一轮下三种位法，各占一个不重叠的 1 MB 窗口（都在 OP-TEE core 段内，
    #      Linux 不用这段）：R13 cfg=0x37、R14 cfg=0x33、R15 cfg=0x0F
    #      → **三个地址全部照常读得到**。
    #
    # 所以排掉的是"R*_CONFIG 位定义猜错了"这条 —— 三种位法一起失败，而且区域
    # 明明写进去也读得回。剩下的嫌疑在"APU 的非安全访问到底经不经 XMPU_DDR"，
    # 以及 R*_MASTER 全 0 是不是被硬件当成"不匹配任何主控"（Xilinx 自己的
    # 例子里从不写 0）。这两条都需要 UG1085 里 XMPU_DDR 的确切寄存器语义，
    # 手上没有可核对的依据，而每猜一轮要把板子停一次。
    #
    # ⚠️ **不留一份不生效的配置在这里。** 它读起来像"DDR 已经被保护了"，
    #    而实际什么都没挡住 —— 那正是独立评审点名的那类夸大，比空着更糟。
    #    docs/SECURITY 里按"未解决的真实缺口"记这一条。

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

    # ---- 页表容量 ----
    # ⚠️ 这一步在**最后**做，因为它会覆盖前面对 platform_def.h 的任何改动 ——
    #    改容量只认这一处，别在别处再写一遍（写了也会被这里冲掉，而且不报错）。
    #
    # bl_regions 现在 6 段（BL31 / CODE / RO_DATA / COHERENT / PL 窗口 /
    # FPD 保护单元），加上 plat_arm_get_mmap() 的平台段，9 已经贴着上限。
    # 越界的表现是 setup_page_tables 的断言在启动早期把 BL31 打死 ——
    # **串口上什么都看不到**（那时 console 还没起），只能靠"起不来"来推断。
    # 所以宁可留富余。
    s = open(PLATDEF, encoding='utf-8').read()
    s = re.sub(r'(#define\s+MAX_MMAP_REGIONS\s+)\d+', r'\g<1>12', s)
    s = re.sub(r'(#define\s+MAX_XLAT_TABLES\s+)\d+', r'\g<1>9', s)
    open(PLATDEF, 'w', encoding='utf-8').write(s)
    for nm in ('MAX_MMAP_REGIONS', 'MAX_XLAT_TABLES'):
        for line in s.splitlines():
            if line.startswith('#define ' + nm):
                print('  ' + line.strip())
    return 0


if __name__ == '__main__':
    sys.exit(main())
