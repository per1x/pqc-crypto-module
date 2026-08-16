# impl_bitstream.tcl —— 从 RTL 一路做到 .bit（非工程模式）
#
#   vivado -mode batch -source hardware/syn/impl_bitstream.tcl
#
# 与 ooc_synth.tcl 的区别：那个是 **out-of-context 模块级**综合，只出资源与
# 时序报告，**产不出 bitstream**（没有顶层、没有 PS、没有 I/O）。这个是真正的
# 实现流程，产物是能烧进 PL 的 .bit。
#
# ============================================================================
# 【为什么用非工程模式（不建 .xpr）】
# ============================================================================
# 工程模式会在磁盘上留一棵只有 Vivado GUI 看得懂的目录树，进不了 git，
# 复现要靠"在界面上点一遍"。非工程模式全部是这一个脚本 —— 它进 git，
# 谁跑都是同一个结果。
#
# 唯一必须用 IP 的地方是 PS（zynq_ultra_ps_e），它由 create_ip 生成，
# 配置写在下面，一样进 git。
#
# ============================================================================
# 【PS 的配置只管 PL 这一侧】
# ============================================================================
# **真正跑在板子上的 PS 配置来自厂家 BOOT.BIN 里的 psu_init，不是这里。**
# 这里的配置只决定两件事：
#   ① 生成的 IP 上有哪些 PL 侧端口（我要 M_AXI_HPM0_LPD 和 pl_clk0）；
#   ② Vivado 按什么频率给这条时钟做时序约束。
# 所以 DDR / MIO 这些一概用默认值 —— 它们只影响 psu_init，而我们不用它。
#
# 已经从 /home/build/petalinux/project-spec/hw-description/system.xsa 里
# 确认过厂家的配置：PSU__USE__M_AXI_GP2=1（HPM0_LPD，32 位）、
# PSU__FPGA_PL0_ENABLE=1、PL0 = 150 MHz。下面照这个填。

set part   xazu3eg-sfvc784-1-i
set root   [file normalize [file dirname [info script]]/..]
set outdir $root/syn/impl
file mkdir $outdir
file mkdir $outdir/ip   ;# create_ip 不会自己建，缺了它直接报 does not exist

create_project -in_memory -part $part
set_property target_language Verilog [current_project]

# ---- PS ----------------------------------------------------------------------
create_ip -name zynq_ultra_ps_e -vendor xilinx.com -library ip \
          -module_name zynq_ultra_ps_e_0 -dir $outdir/ip

set_property -dict [list \
    CONFIG.PSU__USE__M_AXI_GP0 {0} \
    CONFIG.PSU__USE__M_AXI_GP1 {0} \
    CONFIG.PSU__USE__M_AXI_GP2 {1} \
    CONFIG.PSU__MAXIGP2__DATA_WIDTH {32} \
    CONFIG.PSU__USE__S_AXI_GP0 {0} \
    CONFIG.PSU__USE__S_AXI_GP2 {0} \
    CONFIG.PSU__USE__S_AXI_GP3 {0} \
    CONFIG.PSU__FPGA_PL0_ENABLE {1} \
    CONFIG.PSU__CRL_APB__PL0_REF_CTRL__FREQMHZ {150} \
    CONFIG.PSU__FPGA_PL1_ENABLE {0} \
    CONFIG.PSU__FPGA_PL2_ENABLE {0} \
    CONFIG.PSU__FPGA_PL3_ENABLE {0} \
] [get_ips zynq_ultra_ps_e_0]

generate_target {instantiation_template synthesis} [get_ips zynq_ultra_ps_e_0]
synth_ip [get_ips zynq_ultra_ps_e_0]

# ---- 自研 RTL ----------------------------------------------------------------
# 这份清单要跟着 rtl/ 下的子目录一起长 —— 漏一个目录的表现是
# "module 'xxx' not found"，而那要跑完一整轮才看得到（已经栽过一次）。
foreach d {common mlkem mldsa keccak trng sym bus board} {
    set files [glob -nocomplain $root/rtl/$d/*.v]
    if {[llength $files] > 0} {
        read_verilog -sv $files
        puts "读入 rtl/$d：[llength $files] 个文件"
    }
}

# 风扇温控在 rtl/ 之外（hardware/platform/fan_ctrl/），因为它**不是密码逻辑** —— 目录分开
# 是为了让"风扇不碰密码的任何信号"这件事在文件系统上就看得见。
# 但 PL 只有一份、运行时载进去的那一个 bitstream 就是全部，所以它和密码核
# 必须进同一个 bitstream。
set fanfiles [glob -nocomplain $root/platform/fan_ctrl/*.v]
if {[llength $fanfiles] == 0} {
    puts "错误：hardware/platform/fan_ctrl/ 下一个文件都没读到"
    puts "      —— 那样 fan 端口会没有驱动，AA11 悬空，风扇行为不可预测。"
    exit 1
}
read_verilog -sv $fanfiles
puts "读入 hardware/platform/fan_ctrl：[llength $fanfiles] 个文件"

# ---- 管脚约束 ----------------------------------------------------------------
# 必须在 synth_design 之前读：综合要知道 fan 这个端口的 I/O 标准才能推 IOB。
read_xdc $root/syn/constraints/board_pins.xdc

# ---- 综合 --------------------------------------------------------------------
# ⚠️ 不加 -mode out_of_context：这一次要的是能布到器件上的真实现。
# ---- 表征构建开关 ------------------------------------------------------------
# PQC_CHARACTERIZE=1 出的是**表征用**的 bitstream，不是产品形态：
# 风扇阈值压低（这块板热不起来，产品阈值在真硅上够不着）、TRNG 原始噪声
# 抽头打开（SP 800-90B 要调理前的样本）。理由见 zu3eg_hsm_top.v 里那段注释。
# 产物另起名字，免得和产品 bitstream 混在一起 —— 这两份**绝不能搞混**。
set characterize [expr {[info exists ::env(PQC_CHARACTERIZE)]
                        && $::env(PQC_CHARACTERIZE) ne "0"}]
# PQC_DEV_OPEN=1 出的是**开发位流**：功能从机 SECURE_ONLY=0，Linux 能直连
# 跑 KAT。产物另起名字，免得和送检位流混在一起 —— 两者只差一个参数，
# 混了就会照着错的那份写驱动。
set devopen [expr {[info exists ::env(PQC_DEV_OPEN)]
                   && $::env(PQC_DEV_OPEN) ne "0"}]
set bitname [expr {$characterize ? "zu3eg_hsm_char"
                   : ($devopen ? "zu3eg_hsm_dev" : "zu3eg_hsm")}]

# ---- ML-DSA 参数集 -----------------------------------------------------------
# 一份 bitstream 里只有一套（三个核这一版是编译期参数化的）。默认 2 = ML-DSA-87，
# 也就是最大的那一套 —— "塞不塞得下"问的就是它。
# 塞不下时的退路：`PQC_MLDSA_PSET=1`（65）或 `=0`（44）。
# ⚠️ 退到 65/44 是合法的工程取舍，但**必须写清楚上的是哪一套**，
#    含糊成"ML-DSA 全在硬件"就不是了。所以这个数会打进日志与
#    timing_<bitname>.txt，跟着产物走。
set mldsa_pset [expr {[info exists ::env(PQC_MLDSA_PSET)]
                      ? $::env(PQC_MLDSA_PSET) : 2}]
set mldsa_name [lindex {ML-DSA-44 ML-DSA-65 ML-DSA-87} $mldsa_pset]
if {$mldsa_name eq ""} {
    puts "错误：PQC_MLDSA_PSET = $mldsa_pset，只认 0/1/2"
    exit 1
}
# ⚠️ **退到 65/44 的产物必须改名。** 与 dev / char 那两档同一条理由：
#    这几份只差一个参数，长得一模一样，名字一样就必然有人拿错 —— 而拿错的
#    后果是"照着 87 的文档去驱动一份 65 的位流"，pset 对不上会被拒绝启动，
#    而排查方向会指向软件。默认（87）保持 zu3eg_hsm.bit 不动：板上脚本、
#    boot/persist/boot_j1_pl.bif 都按这个名字走，改了会一起坏。
if {$mldsa_pset != 2} {
    append bitname "_mldsa$mldsa_name"
    set bitname [string map {ML-DSA- ""} $bitname]
}
puts "=========================================================="
puts "  ML-DSA 参数集：$mldsa_name （MLDSA_PSET=$mldsa_pset）"
puts "  产物：$bitname.bit"
puts "=========================================================="
set gen_args [list -generic MLDSA_PSET=$mldsa_pset]
if {$characterize} {
    puts "=========================================================="
    puts "  ⚠️ 表征构建：风扇低阈值 + TRNG 原始抽头打开"
    puts "     这一份**不是产品形态**，取完数就换回默认构建"
    puts "=========================================================="
    eval synth_design -top zu3eg_hsm_top -part $part -flatten_hierarchy rebuilt \
                 -verilog_define PQC_CHARACTERIZE=1 $gen_args
} elseif {$devopen} {
    puts "=========================================================="
    puts "  ⚠️ 开发位流：功能从机 SECURE_ONLY=0，普通世界可直连"
    puts "     这**不是**送检形态。产物 zu3eg_hsm_dev.bit"
    puts "=========================================================="
    eval synth_design -top zu3eg_hsm_top -part $part -flatten_hierarchy rebuilt \
                 -verilog_define PQC_DEV_OPEN=1 $gen_args
} else {
    eval synth_design -top zu3eg_hsm_top -part $part -flatten_hierarchy rebuilt \
                 $gen_args
}

# ---- 断言：位流形态与文件名必须一致 ------------------------------------------
# 这条挡的是"照着错的那份文档写驱动"：开发位流与送检位流只差一个参数，
# 名字一样就必然有人拿错。所以直接查网表里防火墙的 SECURE_ONLY 究竟是几，
# 与 bitname 对不上就中止 —— 独立评审的 H1 正是这类错误的后果。
#
# ⚠️ **这条断言一度是空的**：它 `get_cells` 取了 `fw_ns` 之后**从不使用**，
#    后面只有一句 puts —— 没有比较、没有 exit。也就是说这段注释宣称的护栏
#    从来没有生效过，而一条"自称会拦住你"的检查比没有检查更危险：
#    看见它的人会以为形态搞错了会被挡下来。（独立评审复核时抓到的。）
#
# 现在真的查：把功能从机的 SECURE_ONLY **从综合参数里读出来**，与 bitname
# 蕴含的形态比对，对不上就中止，不出 bitstream。
set want_secure [expr {$devopen ? 0 : 1}]
set sec_cells [get_cells -quiet -hier -filter \
                   {REF_NAME =~ "axi4lite_firewall*" && NAME =~ "*u_mlkem*"}]
set got_secure -1
foreach c $sec_cells {
    set v [get_property -quiet SECURE_ONLY $c]
    if {$v ne ""} { set got_secure $v ; break }
}
if {$got_secure == -1} {
    # 查不到就**不放行**：让"检查没跑成"和"检查没通过"一样吵，
    # 而不是安静地变成"检查通过"。
    puts "错误：网表里找不到 mlkem 的防火墙实例，无法核对 SECURE_ONLY —— 中止"
    puts "      （查到的候选：[llength $sec_cells] 个）"
    exit 1
}
if {$got_secure != $want_secure} {
    puts "错误：位流形态与产物名不符 —— SECURE_ONLY=$got_secure，而 $bitname 应当是 $want_secure"
    puts "      开发位流(SECURE_ONLY=0)必须叫 zu3eg_hsm_dev，送检位流(=1)叫 zu3eg_hsm"
    exit 1
}
puts "断言通过：位流形态 = [expr {$devopen ? {开发（SECURE_ONLY=0）} : {送检（SECURE_ONLY=1）}}]（网表实测 $got_secure），产物 $bitname.bit"

# ---- 断言：tamper 那条路必须还活着 -------------------------------------------
# 独立评审 M9：`tamper` 一度恒接 0，于是 key_vault 的一拍全清、防火墙的
# fail-closed、TOCTOU 消窗 —— **一整套已验证过的 RTL 全被综合优化掉了**，
# 而这件事在任何日志里都不会说一声。
#
# 现在它接到 SYSMONE4 的 OT。这条断言查的是**下游**：`tamper_latched` 那些
# 触发器还在不在。它们是纯粹由 tamper 驱动的，源头一旦又变成常量，
# 它们就会消失 —— 于是"死逻辑复发"变成一次构建失败，而不是一份看起来正常
# 的 bitstream。
set tamper_ff [get_cells -quiet -hier -filter {NAME =~ "*tamper_latched*"}]
if {[llength $tamper_ff] == 0} {
    puts "错误：网表里找不到 tamper_latched —— tamper 那条路又被优化掉了"
    puts "      （多半是 zu3eg_hsm_top.v 里的 tamper 又变回常量）"
    exit 1
}
puts "断言通过：tamper 链在网表里（tamper_latched [llength $tamper_ff] 个触发器）"

# ---- 断言：风扇管脚必须真的落在 AA11 -----------------------------------------
# 风扇错了不会崩，只会**安静地让芯片变热** —— 没有任何运行时症状会告诉你
# 约束没读进来。所以在这里直接查器件上的落点。
set fanport [get_ports -quiet fan]
if {[llength $fanport] == 0} {
    puts "错误：顶层没有 fan 端口 —— 风扇不会被驱动"
    exit 1
}
set fanpkg [get_property PACKAGE_PIN $fanport]
if {$fanpkg ne "AA11"} {
    puts "错误：fan 落在 $fanpkg，应当是 AA11（board_pins.xdc 没读进来？）"
    exit 1
}
puts "断言通过：fan -> PACKAGE_PIN $fanpkg / [get_property IOSTANDARD $fanport]"

# ---- 断言：SYSMONE4 的 SIM_DEVICE ---------------------------------------------
# 这条断言也是拿一整轮（三十多分钟）换来的。
# SYSMONE4 的 SIM_DEVICE 默认是 ULTRASCALE_PLUS，而本器件是 ZYNQ_ULTRASCALE。
# Vivado 初始化网表时**自动把它改掉**（只给一条 critical warning），然后在
# **最后一步 write_bitstream 的 DRC ADEF-911** 里因为"它被改过"而拒绝出图。
# 于是综合、布局布线、时序全跑完才失败。在这里查，五分钟内就能知道。
set smon [get_cells -quiet -hier -filter {REF_NAME == SYSMONE4}]
if {[llength $smon] == 0} {
    puts "错误：网表里没有 SYSMONE4 —— 风扇读不到温度，会一直强制满速"
    exit 1
}
set simdev [get_property SIM_DEVICE [lindex $smon 0]]
if {$simdev ne "ZYNQ_ULTRASCALE"} {
    puts "错误：SYSMONE4 的 SIM_DEVICE = $simdev，应当是 ZYNQ_ULTRASCALE"
    puts "      （在 hardware/platform/fan_ctrl/fan_sysmon.v 里显式写死，别靠默认值）"
    exit 1
}
puts "断言通过：SYSMONE4 SIM_DEVICE = $simdev"

# ---- 断言：PS 的时钟/复位输入必须有驱动 --------------------------------------
# **这条断言是拿两次断电换来的。**
# 顶层若引用了后面才声明的 wire（例如把 BUFGCE_DIV 写在 PS 例化之后），
# Vivado **不报错**，而是给那个端口新建一条同名的无驱动网络。综合、布线、
# 时序、bitstream 全部正常，载进板子之后 PS 的 AXI 主口却没有时钟 ——
# CPU 发出的第一笔读永远不返回，整机 wedge，连 sysrq 兜底都跑不起来。
# Icarus 对同样的写法直接报 "Unable to bind wire"；Vivado 的静默才是危险的。
#
# 所以在这里挡住：PS 那几个关键输入，凡是没有驱动的就中止，别浪费一轮上板。
# 注意用 -segments 跨层次追：`get_pins -hier` 会同时抓到层次边界引脚和
# 叶子引脚，而边界引脚的驱动在父层那一段网络上 —— 不跨段追就会误报。
# rlast / bid / rid 也在列：M_AXI_HPM0_LPD 是 AXI4 不是 Lite，这三个是
# **由 PL 驱动、送回 PS** 的响应信号。第一版漏了它们，PS 等不到 rlast，
# 每一笔读都永不返回 —— 而综合、时序、bitstream 全都正常，只在真机上现形。
foreach pinname {maxihpm0_lpd_aclk maxigp2_rlast maxigp2_bid maxigp2_rid} {
    set pin [get_pins -quiet -hier -filter "NAME =~ */inst/$pinname"]
    if {[llength $pin] == 0} {
        set pin [get_pins -quiet -hier "*$pinname"]
    }
    if {[llength $pin] == 0} {
        puts "错误：找不到 PS 的 $pinname 引脚"
        exit 1
    }
    set pp   [lindex $pin 0]
    set nets [get_nets -quiet -segments -of_objects $pp]
    set drv  [get_pins -quiet -of_objects $nets -filter {DIRECTION == OUT}]
    if {[llength $drv] == 0} {
        puts "错误：PS 的 $pinname 无驱动（nets=$nets）"
        puts "      多半是顶层里引用了后面才声明的 wire —— Vivado 不报错，"
        puts "      而是给那个端口新建一条同名的无驱动网络。改成先声明再用。"
        exit 1
    }
    puts "断言通过：$pinname <- $drv"
}

# ---- 环振：放行组合环，降级 DRC ---------------------------------------------
# **少了这一段，前面全部跑通、时序也收敛，最后一步 write_bitstream 会被
# DRC LUTLP-1 挡下来**（第一次跑就是这么栽的）。ring_osc 的组合环是它的
# 工作原理，ring_osc.v 的头注释早写明了要这三条，只是当时没搬进这个流程。
#
# 这几条带条件判断，XDC 里写不了（XDC 不支持 if），只能放在 Tcl 上下文里。
# chain 上带了 DONT_TOUCH，网络名会被保留，可以按名字抓。
set ro_nets [get_nets -hier -quiet -filter {NAME =~ *u_ro*chain*}]
if {[llength $ro_nets] > 0} {
    set_property ALLOW_COMBINATORIAL_LOOPS TRUE $ro_nets
    puts "环振：已对 [llength $ro_nets] 条网络放行组合环"
} else {
    puts "警告：一条环振网络都没抓到 —— 环可能被优化掉了，TRNG 会没有熵"
}
set_property SEVERITY {Warning} [get_drc_checks LUTLP-1]

# 环振输出没有时钟域，到采样触发器是不可时序分析的异步路径；
# 亚稳态由 RTL 里带 ASYNC_REG 的两级同步器吸收。
set sync1 [get_cells -hier -quiet -filter {NAME =~ *sync1_reg*}]
if {[llength $sync1] > 0} {
    set_false_path -to [get_pins -quiet -of_objects $sync1 -filter {REF_PIN_NAME =~ D*}]
    puts "环振：已对 [llength $sync1] 个采样触发器设 false path"
}

report_utilization    -file $outdir/post_synth_utilization.rpt
report_timing_summary -file $outdir/post_synth_timing.rpt
write_checkpoint -force $outdir/post_synth.dcp

# ---- 实现 --------------------------------------------------------------------
# ---- 保持余量：向工具要，而不是等它给 ----------------------------------------
# 实测过的 WHS：不加约束时 +0.001 / +0.010 / +0.013 ns —— 全是正的、Vivado 判
# MET，但都贴着零，而且随 RTL 版本飘。这个数一直在被观察，没有被管理。
#
# 为什么只能走约束这条路（两条弯路都试过）：
#  · 加流水级没用：保持违例的成因是数据到得**太早**，加一级只会再造一条同样
#    短的路。最差那条本来就是 0 逻辑级（mlkem_axi 的 seed_b[207] 直连
#    mlkem_keygen 的 z_r[207]，数据 0.156 ns，目的端时钟晚到 0.405 ns）。
#  · phys_opt_design -hold_fix 没用：它是**建立时间驱动**的，WNS 一旦为正就
#    整个跳过（日志原话 "Skipping all physical synthesis optimizations"）。
#
# 有效的是提高要求：加保持不确定度，route_design 自己的保持修复阶段就会
# 往过短的路径上插延迟直到满足。实测布线过程 WHS 从 -0.150 → -0.027 → +0.010，
# 也就是**真的多买到了约 0.16 ns**。
set hold_unc 0.100
set sysclk [get_clocks -quiet -of_objects [get_pins u_div/O]]
if {[llength $sysclk] == 0} {
    puts "错误：没匹配到 clk_sys，保持不确定度会被静默丢弃"
    exit 1
}
set_clock_uncertainty -hold $hold_unc $sysclk
puts "断言通过：保持不确定度 $hold_unc ns 已加到 [get_property NAME $sysclk]"

opt_design
place_design
phys_opt_design
route_design

# ---- 布线后再专门修一次保持时间 ----------------------------------------------
# 为什么要单独这一步：保持违例的成因是**数据到得太早**，不是太晚。
# 加流水级帮不上忙 —— 那只会再造一条同样短的路。对症的办法只有两个：
# 给数据路径插延迟，或者压小时钟偏斜。
#
# 实测最差的那条是 mlkem_axi 的 seed_b[207] 直连 mlkem_keygen 的 z_r[207]：
# **逻辑级数 0**（寄存器到寄存器，中间什么都没有），数据路径只有 0.156 ns，
# 而目的端时钟比源端晚到 0.405 ns —— 短路径碰上正偏斜，余量只剩 0.010 ns。
# 256 位的种子总线横跨半个片子，这种形状必然如此。
#
# phys_opt_design -hold_fix 正是干这个的：在布线后往过短的数据路径上插延迟。
# 它不改功能、不改 RTL、可重跑，是买回保持余量最省的一步。
# 放在 route_design **之后**：这时的延迟是布线后的真值，插得准。
phys_opt_design -hold_fix
route_design -no_timing_driven -preserve

report_utilization    -file $outdir/post_route_utilization.rpt
report_timing_summary -file $outdir/post_route_timing.rpt
report_power          -file $outdir/post_route_power.rpt
write_checkpoint -force $outdir/post_route.dcp

# ---- 时序判据 ----------------------------------------------------------------
# WNS 为负就不写 bitstream。写出一个时序不收敛的 .bit 再拿去上板，
# 出来的错会像"算法写错了"，查起来极贵 —— 不如在这里停住。
set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
set whs [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -hold]]
puts "=========================================================="
puts "  建立时间 WNS = $wns ns"
puts "  保持时间 WHS = $whs ns"
puts "=========================================================="

# ⚠️ 报告出来的 $whs **已经扣过** $hold_unc 的不确定度，所以真实的物理余量是
#    两者之和。拿 $whs 直接去比地板会算重一次 —— 这一版就栽过：约束明明生效了
#    （布线过程 WHS 从 -0.150 被推到 +0.010），断言却还在报"低于地板"。
set whs_eff [expr {$whs + $hold_unc}]
puts "  有效保持余量 = WHS $whs + 不确定度 $hold_unc = $whs_eff ns"

set fh [open $outdir/timing_$bitname.txt w]
puts $fh "wns=$wns whs=$whs hold_unc=$hold_unc whs_eff=$whs_eff"
puts $fh "mldsa_pset=$mldsa_pset mldsa=$mldsa_name"
close $fh

# ---- 保持时间的**下限**，不只是"非负" ----------------------------------------
# 为什么要有下限：审计那一版实测 WHS = +0.001 ns —— 是正的、sign-off 判通过、
# 上面那条 whs<0 也放行了。但 1 ps 基本等于零，而**保持违例不是降频能救的**：
# 建立余量再大也没用，它只能靠改实现。更要紧的是这个数**一直在飘**
# （不同 RTL 版本实测过 +0.001 / +0.011 / +0.013），也就是说它一直在被观察、
# 没有被管理。一个会自己动的数，只在它变成负数时才报警是不够的。
#
# 所以设一个明确的地板：低于它就中止并说清楚，逼人当场看一眼，
# 而不是让 bitstream 悄悄停在刀片上。
set whs_floor 0.050

if {$wns < 0} {
    puts "错误：建立时间不收敛（WNS = $wns ns），不生成 bitstream"
    exit 1
}
if {$whs < 0} {
    puts "错误：保持时间不收敛（WHS = $whs ns），不生成 bitstream"
    exit 1
}
if {$whs_eff < $whs_floor} {
    puts "错误：有效保持余量 $whs_eff ns 低于地板 $whs_floor ns。"
    puts "      保持违例不是降频能救的 —— 建立余量再大也没用。"
    puts "      要么加大 hold_unc 让布线器插更多延迟，要么在明确知情的前提下"
    puts "      调低 whs_floor。别把它当噪声跳过去。"
    exit 1
}

# ---- bitstream ---------------------------------------------------------------
write_bitstream -force $outdir/$bitname.bit

# fpga_manager 吃的是 .bin（原始比特流，没有 .bit 的头）。
# bitstream 的字节序也要翻 —— 这两件事都由 write_cfgmem 之外的这条路做：
# 2020.1 里最省事的办法是让 bootgen 转，但这里直接生成 .bin 更少一个依赖。
write_cfgmem -force -format BIN -interface SMAPx32 -disablebitswap \
    -loadbit "up 0x0 $outdir/$bitname.bit" $outdir/$bitname

puts "产物："
puts "  $outdir/$bitname.bit"
puts "  $outdir/$bitname.bin  （fpga_manager / fpgautil 用这个）"
exit 0
