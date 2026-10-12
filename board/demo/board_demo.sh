#!/bin/sh
# 板上演示入口 —— 放在 /media/sd-mmcblk1p2/demo/，不在 hsm/ 里面
#
#     ssh root@<板子IP>
#     sh /media/sd-mmcblk1p2/demo/run.sh
#
# ============================================================================
# 【为什么单独放一个目录】
# ============================================================================
# hsm/ 里有六十多个文件：几份 BOOT.BIN 备份、十几个 RESULT_*.txt、各种探针
# 程序。演示的人不该需要在里面找入口。这个目录只放一个东西 —— 就是它。
#
# 真正的程序仍然在 hsm/，这里不复制一份：那是 500 KB 的二进制，复制两份迟早
# 会出现"改了一份忘了另一份"。
#
# ⚠️ 位置必须在 SD 卡上（/media/sd-mmcblk1p2/…）。板子的 rootfs 是 initramfs，
#    写到 / 下面的东西**重启就没了** —— df 报 / 的容量是 0 就是这个意思。
#
# ⚠️ 全程只读：不写寄存器、不动位流、不碰启动槽。第 ④ 段往被拒的地址写，
#    但那些地址按 RAZ/WI 语义本来就把写丢掉。
set -u

HSM=/media/sd-mmcblk1p2/hsm

hr() { echo; echo "──────────────────────────────────────────────────────────"; echo "$*"; echo; }

echo
echo "=========================================================="
echo "  AXU3EGB 后量子密码机 —— 板上演示"
echo "=========================================================="

# ---- 〇 就绪状态：十秒钟说明它是一台开机即用的设备 --------------------------
# 编号用〇，因为下面 hsm/run_demo.sh 自己从 ① 开始 —— 两套编号撞在一起，
# 观众会以为漏了一段。
hr "〇 设备就绪"
if [ -r "$HSM/HSM_STATUS" ]; then
    sed 's/^/  /' "$HSM/HSM_STATUS"
    echo
    echo "  上电到这个状态约 35 秒，全自动，没有任何手工步骤。"
    echo "  PL=crypto 是密码位流已经装进 FPGA；SECMMIO=ok 是安全世界那条通路活着；"
    echo "  TA=ok 是 OP-TEE 里管密钥的那个 TA 已就位（密钥的生成、包裹、装载都在它里面）。"
else
    echo "  读不到 $HSM/HSM_STATUS —— 板子可能还没起完（上电约 35 秒）"
fi

# ---- ②③④ 交给 hsm/run_demo.sh，不复制一份 --------------------------------
if [ -x "$HSM/run_demo.sh" ] || [ -r "$HSM/run_demo.sh" ]; then
    sh "$HSM/run_demo.sh"
else
    echo
    echo "  找不到 $HSM/run_demo.sh"
    exit 1
fi

# ---- ⑤ 管理功能面：槽位 / 密钥 / 备份 / 安全存储 ---------------------------
# 这一段用的是**主机侧那个软件密码机**（pqchsmd + pqchsm-cli + pqchsm-admin），
# 交叉编译后放在 demo/bin/。上面 ①-④ 演的是 FPGA 那半，这一段演的是"一台
# 密码机该有的管理功能"——槽位状态机、SO/User 角色、M-of-N 备份、密钥库。
# 两半合起来才是一台密码机。
if [ -x "$(dirname "$0")/functions.sh" ] || [ -r "$(dirname "$0")/functions.sh" ]; then
    bash "$(dirname "$0")/functions.sh"
else
    hr "⑤ 管理功能面 —— 跳过"
    echo "  找不到 $(dirname "$0")/functions.sh"
fi

# ---- 收尾：把还能往下看的东西指出来 ----------------------------------------
hr "还能看什么"
cat <<'EOF'
  远程调用（在另一台机器上，全程不用 SSH，mTLS 双向证书）：
      ./demo/remote/run.sh <板子IP>

  管理功能面已经在上面第 ⑤ 段跑过了。主机上也能单独跑（不需要板子）：
      ./demo/functions/run.sh

  这块板上更细的证据（原始输出，未经整理）：
      ls /media/sd-mmcblk1p2/hsm/RESULT_*.txt

  对称密钥寄存器没有读出通路（密钥装进 PL、密文算对的同时，扫遍密钥寄存器与
  对称核的地址窗口，密钥字一个都不出现），daemon 在跑时也能做：
      /media/sd-mmcblk1p2/hsm/d7bench
EOF
echo
