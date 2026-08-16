/* bl33_shim.S —— 救砖用的最小引导垫片：在 EL3 里自己降到 EL2，跳进内核
 *
 * ============================================================================
 * 【为什么不用 BL31 —— 一次靠反汇编定位的死锁】
 * ============================================================================
 * 最初的救砖路径是 FSBL → BL31 → 内核，交给 BL31 去做 EL3→EL2 的降级。
 * 结果核永远停在 0xfffefbfc、cpsr 显示还在 EL3。反汇编 bl31.elf 才看清：
 *
 *     fffefbf8:  ldr w2, [x0]        ; 读 IPI 观察寄存器
 *     fffefbfc:  tst w1, w2
 *     fffefc00:  b.ne fffefbf8       ; 位没清就一直转   <-- ipi_mb_notify
 *
 * BL31 在**等 PMU 固件应答 IPI**。而 PMUFW 正常是 FSBL 从 BOOT.BIN 里解析出来
 * 装进 PMU 的 —— 救砖时 BOOT.BIN 恰恰是坏的，JTAG 喂进去的 FSBL 手上没有镜像，
 * PMU 里就是空的。想用 JTAG 直接灌 PMUFW 也不行：PMU 目标回 "Invalid context"。
 *
 * 也就是说：**BL31 这条路在"BOOT.BIN 坏了"这个前提下是自相矛盾的** ——
 * 它依赖的东西正好是坏掉的那个。
 *
 * 所以把它拿掉。EL3→EL2 的降级本来就只是几条系统寄存器写加一条 eret，
 * 用不着一整个固件。少一个依赖，救砖路径就少一处能坏的地方。
 *
 * ============================================================================
 * 【代价：没有 PSCI】
 * ============================================================================
 * BL31 同时提供 PSCI，没有它 Linux 起不了从核（设备树里 psci method="smc"
 * 会失败）。**这是有意的取舍**：救砖只需要一个能跑的单核 Linux 去把
 * BOOT.BIN 恢复回来，不需要 SMP。内核会为从核报几行错，然后照常起来。
 */
    .section .text
    .global _start
_start:
    /* SCR_EL3：把下一级放到非安全 EL2、且是 aarch64
     *   bit0  NS  = 1   非安全
     *   bit7  SMD = 1   屏蔽 SMC（没有 BL31 接管，别让 SMC 陷进空处理）
     *   bit8  HCE = 1   允许 EL2 用 HVC
     *   bit10 RW  = 1   下一级是 aarch64
     */
    mov     x0, #0x581
    msr     scr_el3, x0
    isb

    /* SPSR_EL3：eret 之后落到 EL2h，DAIF 全屏蔽（内核入口要求关中断） */
    mov     x0, #0x3c9
    msr     spsr_el3, x0

    /* 别让 FP/SIMD 在 EL2/EL1 被陷住 */
    msr     cptr_el3, xzr
    isb

    /* ⚠️ 这里曾试图配 GIC，最后放弃 —— 记下为什么，省下一个人的功夫。
     *
     * 正常启动里 BL31 会把中断划到 Group 1（非安全）并配好 arch timer 的
     * EL3 侧。绕过 BL31 之后没人做这件事，内核卡在
     * "xor: measuring software checksum speed"（jiffies 不前进 = 定时器
     * 中断进不来）。
     *
     * 在这里写过 GICD_IGROUPR 全 1，用一段自证把它读回 DDR 让 JTAG 核对，
     * 读回是 **0xFDFFFFFF** —— bit 25 clear 不掉，那是 arch generic timer
     * 的 PPI，**被硬件钉死在 Group 0（安全）**。要让内核收到它，得重建
     * BL31 的 EL3 timer/GIC 安全侧配置，几百行，等于在救砖垫片里重写半个
     * BL31。而这一步换来的只是"自动 cp 恢复 BOOT.BIN"，省一条命令，不值。
     *
     * 所以 J0 的结论：**JTAG 全自动把内核引导到 EL2/EL1（已实测），恢复
     * BOOT.BIN 半自动**（板子引导到任意可控态后一条 cp）。核心价值——
     * "写坏 golden 不等于砖"——已经拿到，这就是做 J1/J2 的安全网。
     */

    /* ========================================================================
     * GIC：把全部中断放进 Group 1（非安全），否则非安全 EL1 的内核收不到
     * ========================================================================
     * ⚠️ 这一段当初**被放弃过**，理由是"GICD_IGROUPR 的 bit 25（arch timer
     *    的 PPI）清不掉，硬件把它钉死在 Group 0"。那个理由现在**不成立了**：
     *    救砖设备树已经把 arch timer 整个禁掉、内核改用 TTC（见
     *    build_rescue.sh 里那段），所以 bit 25 是什么状态都无所谓。
     *
     *    症状是这么找到的：JTAG 从 DDR 里读内核的 log ring buffer，日志停在
     *    rcu_init() 之后 —— 下一步正是 init_IRQ()，也就是 GIC 初始化。
     *
     * GIC-400 @ ZynqMP：GICD 0xF9010000，192 个 SPI ⇒ IGROUPR0..5 六个寄存器。
     * 这一步只能在 EL3 做（JTAG 的内存访问是非安全的，写这组寄存器会
     * "Memory write error ... DAP status 0x30000021"，那也是实测过的）。
     */
    mov     x5, #0xF9010000
    movk    x5, #0xF901, lsl #16
    add     x5, x5, #0x80               /* GICD_IGROUPR0 */
    mov     w6, #-1                     /* 0xFFFFFFFF：全部归 Group 1 */
    mov     w7, #6                      /* IGROUPR0..5 覆盖 192 个中断 */
1:  str     w6, [x5], #4
    subs    w7, w7, #1
    b.ne    1b

    /* GICD_CTLR：把 Group0/Group1 都使能（bit0 EnableGrp0、bit1 EnableGrp1） */
    mov     x5, #0xF9010000
    movk    x5, #0xF901, lsl #16
    ldr     w6, [x5]
    orr     w6, w6, #3
    str     w6, [x5]
    dsb     sy
    isb

    /* 内核入口 → ELR_EL3；x0 = 设备树，x1..x3 = 0（arm64 启动约定） */
    ldr     x4, .Lkernel
    msr     elr_el3, x4
    ldr     x0, .Ldtb
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    isb
    eret                        /* 到此进 EL2 的内核，不返回 */

    .align 3
.Ldtb:
    .quad   DTB_ADDR
.Lkernel:
    .quad   KERNEL_ADDR
