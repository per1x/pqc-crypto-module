#!/bin/bash
# build_rescue.sh —— 生成 jtag_rescue.tcl 需要的三个产物
#
#   在**构建机**上跑：bash build_rescue.sh
#   产物：/tmp/shim.bin  /tmp/rescue.dtb  /tmp/rescue_initramfs.gz
#
# ============================================================================
# 【为什么这个脚本必须存在】
# ============================================================================
# jtag_rescue.tcl 引用了这三个文件，而它们**一个都不在仓库里** —— 于是
# "救砖流程已脚本化"这句话在换一台机器上是不成立的：脚本在，材料不在。
# 独立评审把 J0（建立恢复能力）列为**所有启动侧加固的许可证**，而一个
# 复现不出来的许可证等于没有。
#
# 三个产物都是**从构建机上已有的东西生成**，不引入任何新的二进制依赖：
#   shim.bin            ← board/recovery/bl33_shim.S（本仓库）
#   rescue.dtb          ← petalinux 的 system.dtb 改几个节点
#   rescue_initramfs.gz ← petalinux 的 rootfs.cpio 后面拼一个小 cpio
#
# ============================================================================
# 【⚠️ 救砖内核为什么要禁掉 arch timer】
# ============================================================================
# 垫片自己做 EL3→EL2 降级、**不经 BL31**（理由见 bl33_shim.S：BL31 要等
# PMU 固件应答，而 PMUFW 正是从坏掉的 BOOT.BIN 里来的，那条路自相矛盾）。
# 代价是 GICD_IGROUPR 的 bit 25 —— arch generic timer 的 PPI —— 被硬件钉死
# 在 Group 0（安全侧），非安全 EL1 的内核**收不到它的时钟中断**。
# 后果不是"慢"，是**调度器起不来、userspace 根本不运行**，于是恢复 BOOT.BIN
# 只能人工敲命令（上一轮的结论就停在这里：内核起得来、恢复是半自动）。
#
# 这里换一条路：**在救砖设备树里把 arch timer 关掉**，让内核回落到 TTC
# （ZynqMP 的 ttc0~3 在 PS 里，与安全侧无关，设备树里本来就有）。
# 时钟源换了，调度器就活了，userspace 能跑，恢复才谈得上全自动。
#
# ⚠️ 这份设备树**只给救砖用**。正常启动仍然走 BL31 + arch timer，
#    别把它装到 SD 上去。产物名字里带 rescue 就是为了不混。
set -euo pipefail

IMG=${IMG:-/home/build/petalinux/images/linux}
OUT=${OUT:-/tmp}
SRC=$(cd "$(dirname "$0")" && pwd)

# 地址布局：**必须与 jtag_rescue.tcl 里的 dow 地址逐字一致**
KERNEL_ADDR=0x00080000
DTB_ADDR=0x02000000
INITRD_ADDR=0x04000000

echo "=== 1/3 引导垫片 shim.bin ==="
aarch64-linux-gnu-gcc -c -x assembler-with-cpp \
    -DDTB_ADDR=$DTB_ADDR -DKERNEL_ADDR=$KERNEL_ADDR \
    -o "$OUT/shim.o" "$SRC/bl33_shim.S"
aarch64-linux-gnu-ld -Ttext=0x08000000 -o "$OUT/shim.elf" "$OUT/shim.o"
aarch64-linux-gnu-objcopy -O binary "$OUT/shim.elf" "$OUT/shim.bin"
ls -l "$OUT/shim.bin"

echo "=== 2/3 救砖 initramfs ==="
# ⚠️ **解包再打包，不要用"拼接两个 cpio"那一招。**
# 第一版是 `cat rootfs.cpio our.cpio | gzip`，内核起到了 EL1h、IRQ 也使能，
# 但 userspace 一直不跑（RESCUE_DONE.txt 没有、DDR 标记也没插上）。
# 内核能不能在 `TRAILER!!!` 之后接着解下一段，取决于归档之间的补齐，
# 不同 cpio 实现补得不一样 —— 这是个**看起来能用、实际看命**的做法。
# 而这块板的串口在 USB 层是坏的，看不到 console，于是"为什么不跑"完全不可观测。
# 救砖路径上不能留这种东西：解包再打包多花十几秒，换来确定性。
#
# ⚠️ **覆盖 /init，不要用 rdinit=**。它是内核找的第一个用户程序，
# 不需要命令行配合，也不受 CONFIG_CMDLINE_FORCE 之类影响。
RD=$OUT/rescue_root
rm -rf "$RD"; mkdir -p "$RD"
( cd "$RD" && cpio -idm --quiet < "$IMG/rootfs.cpio" )
# 原来的 /init 多半是只读的（或是个符号链接），直接覆盖会 Permission denied
rm -f "$RD/init"
cat > "$RD/init" <<'RESCUE'
#!/bin/sh
# 救砖 init —— **覆盖发行版的 /init**，内核起来找的第一个用户程序就是它
#
# 只做一件事：把一份已知好的 BOOT.BIN 放回去，然后停住等断电。
# **不依赖网络、不依赖串口** —— 救砖环境里少一个依赖就少一处能坏的地方。
exec >/dev/kmsg 2>&1
# ⚠️ 先在 DDR 里插一面旗，再做别的：这块板的串口坏着、救砖时也没有网，
# "userspace 到底跑没跑"本来完全不可观测。这个标记可以事后用 JTAG 读，
# 于是"没跑起来"与"跑了但 SD 写不进去"能分开 —— 上一版就卡在分不开。
[ -x /bin/busybox ] && /bin/busybox devmem 0x0F000000 32 0x5245534D   # "RESM"
echo "RESCUE: init 起来了"
/bin/mount -t proc none /proc || true
/bin/mount -t sysfs none /sys || true
[ -x /sbin/mdev ] && /sbin/mdev -s

i=0
while [ $i -lt 20 ]; do
    [ -b /dev/mmcblk1p1 ] && break
    sleep 1; i=$((i+1))
done

mkdir -p /mnt/p1 /mnt/p2
/bin/mount -t vfat /dev/mmcblk1p1 /mnt/p1 || echo "RESCUE: p1 挂不上"
/bin/mount        /dev/mmcblk1p2 /mnt/p2 || echo "RESCUE: p2 挂不上"

# 恢复源按优先级：先用"上一次已知好的工作镜像"，没有才退回黄金镜像。
SRC=""
for c in /mnt/p2/hsm/BOOT_KNOWN_GOOD.BIN /mnt/p2/hsm/BOOT_GOLDEN_BACKUP.BIN \
         /mnt/p1/BOOT_GOLDEN.BIN; do
    [ -f "$c" ] && { SRC="$c"; break; }
done

if [ -n "$SRC" ]; then
    echo "RESCUE: 用 $SRC 覆盖 /mnt/p1/BOOT.BIN"
    /bin/cp -f "$SRC" /mnt/p1/BOOT.BIN && sync
    echo "RESCUE: 恢复完成"
    {
        echo "救砖成功 $(cat /proc/uptime | cut -d' ' -f1)s"
        echo "恢复源 = $SRC"
        echo "md5(BOOT.BIN) = $(md5sum /mnt/p1/BOOT.BIN 2>/dev/null | cut -d' ' -f1)"
        echo "内核 = $(cat /proc/version)"
    } > /mnt/p2/hsm/RESCUE_DONE.txt
    sync
else
    echo "RESCUE: 一个可用的恢复源都没找到"
fi

echo "RESCUE: 完事，停住等断电"
while true; do sleep 60; done
RESCUE
chmod +x "$RD/init"
( cd "$RD" && find . | cpio -o -H newc --quiet ) | gzip -9 > "$OUT/rescue_initramfs.gz"
ls -l "$OUT/rescue_initramfs.gz"

echo "=== 3/3 救砖设备树 rescue.dtb ==="
INITRD_END=$(( INITRD_ADDR + $(stat -c %s "$OUT/rescue_initramfs.gz") ))
dtc -I dtb -O dts -o "$OUT/rescue.dts" "$IMG/system.dtb" 2>/dev/null
python3 - "$OUT/rescue.dts" "$INITRD_ADDR" "$INITRD_END" <<'PY'
import re, sys
path, start, end = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3])
s = open(path).read()

# ① chosen：指到 initramfs，并且用 rdinit 直接拉救砖脚本（不走发行版 init）
chosen = re.search(r"\tchosen \{(.*?)\n\t\};", s, re.S)
assert chosen, "设备树里没有 chosen 节点"
body = chosen.group(1)
body = re.sub(r'\n\t\tbootargs = .*?;', '', body, flags=re.S)
# ⚠️ **救砖内核不要 console。** 内核日志停在一条长消息的**中间**
# （"Memory: ... 704K" 之后戛然而止），那是 console 驱动卡在 UART 里自旋的
# 典型样子：earlycon 逐字节轮询 TX，FIFO 排不空就永远转，printk 不返回，
# 整个启动就停在那儿。而这块板的串口本来就是坏的（USB 层就坏，见
# board-axu3egb-access 那条）。
# 救砖不需要 console —— 日志是用 JTAG 从 DDR 里读的（dumplog.tcl）。
# 少一个依赖就少一处能挂住的地方，与"不经 BL31"、"不经 U-Boot"同一个道理。
body += ('\n\t\tbootargs = "clk_ignore_unused";'
         '\n\t\tlinux,initrd-start = <0x%x>;'
         '\n\t\tlinux,initrd-end = <0x%x>;' % (start, end))
s = s[:chosen.start(1)] + body + s[chosen.end(1):]

# ② arch timer 关掉 —— 理由见本脚本文件头那段
def disable(compat):
    global s
    n = 0
    for m in list(re.finditer(r"\n\t(\S[^\n]*?) \{\n(.*?)\n\t\};", s, re.S)):
        if compat in m.group(2) and 'status = "disabled"' not in m.group(2):
            s = s[:m.end(2)] + '\n\t\tstatus = "disabled";' + s[m.end(2):]
            n += 1
            break
    return n

assert disable('"arm,armv8-timer"'), "没找到 arch timer 节点"

# ③ **psci 节点整个删掉** —— 这一条是用内核 log ring buffer 查出来的
#
# 症状：内核起得来（PC 在内核空间、CPSR=EL1h、IRQ 使能），但 userspace 永远
# 不跑。用 JTAG 从 DDR 里把 __log_buf 读出来之后一眼看到：日志停在
#     cma: Reserved 256 MiB at 0x0000000050000000
# 之后一条都没有。arm64 的 setup_arch() 里 bootmem_init()（打这条 CMA 日志）
# **紧接着就是 psci_dt_init()**。
#
# 而救砖垫片把 BL31 整个拿掉了，并且 SCR_EL3.SMD=1 **屏蔽了 SMC**（那是有意的：
# 没有 BL31 接管，不能让 SMC 陷进空处理）。于是设备树里 psci 的 method="smc"
# 一发出去就是未定义指令 —— 早期就死，连下一条日志都来不及打。
#
# bl33_shim.S 文件头写的"内核会为从核报几行错，然后照常起来"是**乐观了**：
# 报错的前提是 PSCI 调用能返回一个错误码，而这里它根本回不来。
#
# 删掉 psci 节点，内核就不会去发那个 SMC。代价是起不了从核 —— 救砖只需要
# 一个能跑的单核 Linux 去把 BOOT.BIN 放回去，本来就不需要 SMP。
m = re.search(r"\n\tpsci \{\n.*?\n\t\};", s, re.S)
assert m, "设备树里没有 psci 节点"
s = s[:m.start()] + s[m.end():]

open(path, "w").write(s)
print("  chosen / arch timer / psci 三处都改好了")
PY
dtc -I dts -O dtb -o "$OUT/rescue.dtb" "$OUT/rescue.dts" 2>/dev/null
ls -l "$OUT/rescue.dtb"

echo
echo "三个产物都在 $OUT，可以跑 xsct $SRC/jtag_rescue.tcl 了"
echo
cat <<'STATUS'
⚠️ 当前状态（2026-08-17，如实记）：
   · JTAG 通路、FSBL、DDR、装 PL、解隔离、装内核/设备树/initramfs、跳垫片
     —— 七步全部实测通过；
   · 内核**确实起来了**：停下来读 PC 在内核地址空间、CPSR 的 m=5（EL1h）、
     IRQ 已使能（说明禁 arch timer 让它回落 TTC 这一步是有效的）；
   · **但 userspace 至今没跑起来**：RESCUE_DONE.txt 没出现，init 里那个
     写 0x0F000000 的 DDR 标记也没插上。两种 initramfs 打法（拼接 cpio、
     解包再打包）都试过，结果一样。
   · 根因**尚未定位**，而且不好定位：这块板的串口在 USB 层是坏的，
     看不到 kernel console，earlycon 的输出无处可读。
     下一步应当是用 JTAG 从 DDR 里把内核 log ring buffer 读出来
     （需要 System.map 里的 __log_buf 地址），而不是继续换打法猜。

   所以 J0 现在的准确说法是：
     **能救"多重启动槽写坏"这一类 —— 而且全程不用碰板子**
     （已实测：rst -system + rst -processor + con 就能让它按 SD 重新启动）；
     **"BOOT.BIN 本身写坏"那一类还没有演练成功** —— 内核起得来，
     但自动恢复的最后一步没打通。别把它当成"救砖已闭环"。
STATUS
