#!/bin/sh
# run_demo.sh —— 一条命令跑完整个演示
#
#   在板子上：      sh run_demo.sh
#   在另一台机器上： sh run_demo.sh <板子IP> <凭据目录>
#
# ============================================================================
# 【演示要证明三件事，缺一件这台机器就不算密码机】
# ============================================================================
#   ① **算得对**：调用 → FPGA 里的密码核算 → 结果对上官方向量。
#      光"跑通了"不够，必须对上 NIST ACVP / GB-T 的字节。
#
#   ② **密钥在 TEE 里管**：后量子私钥的种子与对称密钥都由 OP-TEE 里的 TA
#      生成、包裹、装载；应用和 daemon 手里只有句柄与密文 blob。私钥每次在
#      FPGA 里现展开、算完即擦；对称密钥在 FPGA 的密钥寄存器里没有任何读出路径。
#
#   ③ **绕过接口也够不着**：送检形态下直接扫 PL 地址空间，五个功能核全部读回 0；
#      演示形态下功能核是开的，但对称密钥寄存器的装载口仍然只认安全世界。
#
# ③ 是最容易被略过、也最要紧的一条：①② 都是"按规矩用"的结果，
# 而 ③ 是"不按规矩用"的结果。只演示前两条，等于只证明了正门是锁着的。
#
# ============================================================================
# 【顺带展示一条反直觉的性质：怎么打都崩不了】
# ============================================================================
# 第 ④ 步故意往被拒的地址狂写三万多笔。在改成 RAZ/WI 之前，
# **第一笔就足以让内核 panic**（写是 posted 的，DECERR 以 SError 回来）。
# 现在它只是安静地什么也没发生。这一条对演示的意义比听起来大：
# 它意味着现场随便谁乱敲都弄不坏这台机器。
set -u

REMOTE=""
if [ $# -ge 2 ]; then
    REMOTE="$1 $2"
fi

D=$(dirname "$0")
[ -f "$D/sdf_demo" ] || D=/media/sd-mmcblk1p2/hsm

hr() { echo; echo "──────────────────────────────────────────────────────────"; echo "$*"; echo; }

hr "① + ② 算得对 & 密钥在 TEE 里管、运算在 FPGA 里做"
# shellcheck disable=SC2086
"$D/sdf_demo" $REMOTE || { echo "演示失败"; exit 1; }

if [ -n "$REMOTE" ]; then
    cat <<'EOF'

──────────────────────────────────────────────────────────
③ ④ 要在板子上跑（它们要直接摸 /dev/mem，那是本机的事）

    ssh root@<板子IP>
    sh /media/sd-mmcblk1p2/hsm/run_demo.sh

EOF
    exit 0
fi

# ---- 先认形态：③④ 的判据只在送检形态下成立 ---------------------------------
# 板子默认起的是**演示形态**（zu3eg_hsm_dev.bit，SECURE_ONLY=0），为的是让
# 普通世界能直接跑 KAT。而 ③ 问的是"门关上了没有" —— 在这个形态下门本来就
# 是开的，跑它只会打出五行 FAIL，看着像板子坏了，实际只是拿错了尺子。
#
# 判据用两个地址，两个都要看：
#   · 槽 5 风扇观测口（offset 0）永远 SECURE_ONLY=0 —— 它证明 /dev/mem 这条
#     路本身是通的。少了它，一根拔掉的线也会"看起来像门关着"。
#   · 槽 0 trng 的 VERSION —— 送检形态读回 0，演示形态读回 0x0001_0000。
#
# ⚠️ VERSION 在 **offset 0x20**，不是 0x00（0x00 是 CTRL，读回 ENABLE=1）。
#    寄存器表见 docs/REGISTERS.md；hsm_secneg.c 读的也是 PL_BASE+0x20。
#    这里最初写成 0x8000_0000，于是恒为 unknown —— 判据本身要对得上表。
FAN_VERSION=0x80050000
TRNG_VERSION=0x80000020

FORM=unknown
if command -v busybox >/dev/null 2>&1; then
    FAN=$(busybox devmem $FAN_VERSION 2>/dev/null || echo ERR)
    TRNG=$(busybox devmem $TRNG_VERSION 2>/dev/null || echo ERR)
    case "$FAN:$TRNG" in
    0x00010000:0x00000000) FORM=secure ;;
    0x00010000:0x00010000) FORM=open ;;
    esac
fi

case "$FORM" in
secure)
    hr "③ 绕过接口直接扫地址：读不出密钥，功能核的 VERSION 全部读回 0"
    "$D/hsm_secneg" >/dev/null 2>&1
    cat "${SECNEG_OUT:-/media/sd-mmcblk1p2/hsm/RESULT_secneg.txt}"
    ;;
open)
    hr "③ 绕过接口：演示形态下唯一仍然关着的门 —— 对称密钥的装载口"
    cat <<'EOF'
当前跑的是**演示形态**（zu3eg_hsm_dev.bit，SECURE_ONLY=0）：五个功能核
（TRNG、对称密钥寄存器、对称核、ML-KEM、ML-DSA）对普通世界是开着的，Linux 能直接
驱动 KAT。板子开机默认起的就是这一份。

但有一样东西**不跟这个开关走**：对称密钥寄存器的装载口只认安全世界的事务
（种子暂存口也一样）。下面绕过 TEE、用普通世界直接往装载口写一把密钥：
EOF
    echo
    if [ -x "$D/d7probe" ]; then
        "$D/d7probe" | sed 's/^/  /'
    else
        echo "  （缺 $D/d7probe，跳过）"
    fi
    cat <<'EOF'

想看"整扇门都关上"的样子，换成送检形态（zu3eg_hsm.bit，SECURE_ONLY=1）：

    sh /media/sd-mmcblk1p2/hsm/plharness.sh /media/sd-mmcblk1p2/hsm/pay_d7_sec.sh
    cat /media/sd-mmcblk1p2/hsm/d7seclog.txt

它会装送检位流、从普通世界扫一遍（全部读回 0）、再经安全世界跑完整 KAT 与
sdf_demo，最后装回出厂位流。跑完重启一次（hsmreboot.sh）回到演示形态。

⚠️ 换位流必须走 plharness.sh —— 它会先把带活 AXI 主口的驱动解绑。直接
   fpgautil 重配会把总线挂死，那时候 sysrq 也救不回来，只能断电。

两个形态各自证明的东西不同，缺一不可：
  · 演示形态证明**算得对**（普通世界直连核，逐字节对上官方向量）；
  · 送检形态证明**够不着**（同一批地址，普通世界全部读回 0，
    而安全世界经 EL3 白名单照样跑通全套 KAT）。
EOF
    ;;
*)
    # 认不出来就说认不出来。**不要**默默当成某一种 —— ③④ 的判据是形态相关的，
    # 猜错形态会让一份看着正常的报告说出反的结论，比直接报"没认出来"糟得多。
    hr "③ 跳过 —— 认不出当前是哪个位流形态"
    cat <<EOF
读 ${FAN_VERSION}（风扇观测口）得到 ${FAN:-读不到}
读 ${TRNG_VERSION}（trng VERSION）得到 ${TRNG:-读不到}

预期是这两种之一：
  0x00010000 / 0x00000000  → 送检形态（SECURE_ONLY=1）
  0x00010000 / 0x00010000  → 演示形态（SECURE_ONLY=0）

风扇口读不到真值，多半是 PL 没配上或 /dev/mem 这条路不通（先看
cat /media/sd-mmcblk1p2/hsm/HSM_STATUS 里的 PL= 那行）。③ 的判据依赖形态，
认不出来就不跑 —— 猜一个只会给出一份可信度为零的报告。
EOF
    ;;
esac

hr "④ 往被拒的地址狂写三万多笔：板子不会崩"
"$D/hsm_nocrash" >/dev/null 2>&1
cat "${NOCRASH_OUT:-/media/sd-mmcblk1p2/hsm/RESULT_nocrash.txt}"

hr "演示结束。板子还活着 —— 这句话本身是第 ④ 步的结论。"
uptime
