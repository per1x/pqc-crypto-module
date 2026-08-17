#!/bin/sh
# verify_after_por.sh —— 一次断电（POR）之后立刻跑这一支，把两件事一次收掉。
#
# 为什么必须"POR 之后立刻"：XMPU 的 ERR_ADDR / ERR_MASTER / ISR 是**粘的**
# （锁存第一次违规就不再更新），只有 POR 能清。所以"APU 的访问到底有没有到达
# XMPU_DDR"这个判决性问题**一块板上只有一次干净的机会** —— 谁先让别的主控违规
# 一次，这次机会就没了。因此本脚本第一步就做它，在任何 DMA 测试之前。
#
# 用法（从 Mac 经构建机跳板；串口只读，不要用串口下这些命令）：
#   ssh -p 2222 root@192.168.50.191 \
#     'ssh -o HostKeyAlgorithms=+ssh-rsa root@192.168.50.175 "sh /media/sd-mmcblk1p2/hsm/verify_after_por.sh"'
#
# 关联：docs/SECURITY.zh-CN.md 的两节「2026-08-17（第二会话）」

set -u
HSM=/media/sd-mmcblk1p2/hsm
OUT=$HSM/RESULT_after_por.txt
exec > "$OUT" 2>&1

echo "=== POR 之后的判决性验证 ==="
echo "时间：$(date)"
echo

echo "--- 0. 健康检查 ---"
cat $HSM/HSM_STATUS
echo "fpga_manager: $(cat /sys/class/fpga_manager/fpga0/state)"
echo

echo "--- 1. XMPU：先确认 ERR 寄存器是干净的（POR 刚清过）---"
$HSM/protunit_probe | sed -n '/XMPU_DDR0..5 状态/,/^$/p'
echo

echo "--- 2. 用 APU 读一串跨越交织条带的地址 ---"
# DDR 地址在六个 XMPU_DDR 实例之间交织。读 24 个跨 1 MB 步长的地址，
# 足以覆盖全部六个实例，无论交织粒度是多少。
i=0
while [ $i -lt 24 ]; do
    a=$((0x60000000 + i * 0x100000))
    printf "  0x%08x = %s\n" "$a" "$(/sbin/devmem $a 32 2>&1)"
    i=$((i + 1))
done
echo

echo "--- 3. 再 dump 一次六个实例 ---"
$HSM/protunit_probe | sed -n '/XMPU_DDR0..5 状态/,/^$/p'
echo

cat <<'EOF'
--- 怎么判 ---
把第 1 步与第 3 步的 ERR_ADDR / ERR_MASTER 对照：

  · 六个实例**全部**锁存到 APU 的主控号
      → APU 的访问到达了 XMPU_DDR、也被检测为违规，
        纯粹是**没有 gate**（XAPP1320：XMPU 只打毒标记，由终点掐掉）。
        修法：开按属性毒化（CTRL.POISONCFG + POISON.ATTRIB），要改 BL31。

  · 仍有实例是 0（尤其第 2 步明明读遍了所有条带）
      → APU 的访问**根本没到达 XMPU_DDR**，是通路问题，
        不是配置问题 —— 那么 XMPU_DDR 在这颗芯片上就挡不住 APU，
        文档里必须如实写成架构边界。

⚠️ 第 2 步的读**本身就会把粘性寄存器用掉**。这一支只有一次有效运行；
   要重来必须再断一次电。
EOF

echo
echo "结果已写入 $OUT"
