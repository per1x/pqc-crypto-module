# pay_syzero.sh —— 批 1 第 3 项：**daemon 真的发 SY_CTRL 擦对称金库**
#
# ============================================================================
# 【为什么必须是 A/B，而不是"新 daemon 跑完看一眼"】
# ============================================================================
# ⚠️ 判据是 **SY_STATUS 的低三位**（{key_ready[2], op_done[1], busy[0]}），
#    **不是 KVOK(bit3)**。第一版拿 KVOK 当判据，那是错的，理由有两层：
#
#      · kv_valid 是 sym_axi 的**输入**（sym_axi.v:81），来自 key_vault，
#        而 SY_CTRL[0] 的 zeroize_all 只接到三个密码核（aes/sm4/sm3），
#        **接不到金库**。所以 SY_CTRL 擦完 KVOK 本来就不会变。
#      · 更要紧的是：daemon 里写得清清楚楚"对称密钥的 key_vault **不在这里
#        擦**"—— 它是应用显式 ImportKey 装进去的，生命周期归调用方（PKCS#11
#        那侧有对象销毁语义）。所以"会话结束 KVOK 还是 1"是**设计如此**，
#        拿它当缺陷判据等于给正确行为判了错。
#
#    Z-05 说的是另一件事：sym_axi 一直有擦三个核的能力（CTRL[0]，把展开的
#    轮密钥擦掉），而 daemon 从来没发过。所以要看的是核的状态位。
#
# 两版 daemon 各跑一遍同一个演示：
#   A 旧（pre_b1）：会话结束后核里还留着上一次运算的痕迹（op_done=1）；
#   B 新（b1new）：结束后应当回到**刚 zeroize 过**的那个状态。
#
# 同一轮里放一个**手工 ZEROIZE 作标定**：先记下"刚擦过长什么样"，再拿它去
# 对 B_new。判据是在这一轮现场标出来的，不是我事先猜的一个常数。
#
# ============================================================================
# ⚠️ 【第一版这个脚本测了个寂寞，教训写在这里】
# ============================================================================
# 第一版用 `busybox pkill -f pqchsm_fpgad` 停 daemon。**这个 busybox 里没有
# pkill、也没有 pgrep**（`busybox --list` 里查得到），于是那两行是空操作：
# 开机起的那个旧 daemon 一直占着 9797，我起的两个都因为端口被占当场退出，
# 而 sdf_demo 照样连上了**开机那一个**、照样 16 个 ✅、rc=0。
#
# **两轮测的其实是同一个进程**，于是两轮结果一模一样 —— 而这个"一模一样"
# 看起来正像"新旧没差别"，是个会把人引向错误结论的假阴性。
#
# 所以现在两道硬闸：① 按 PID 逐个杀，杀完确认 ps 里一个不剩；
# ② 起完之后**确认 9797 归本轮这个二进制所有**，对不上就当场退出，
# 绝不让它继续跑出一份看似有效的结果。
D=$(dirname "$0")
[ -f "$D/sdf_demo" ] || D=/media/sd-mmcblk1p2/hsm
OUT=$D/syzerolog.txt
SY_CTRL=0x80020004
SY_STATUS=0x80020008
: > $OUT
fail=0

pids() { ps | grep pqchsm_fpgad | grep -v grep | busybox awk '{print $1}'; }

kv() {
    V=$(busybox devmem $SY_STATUS 2>/dev/null || echo ERR)
    case "$V" in
      ERR) echo "SY_STATUS=读不到" ;;
      *)   echo "SY_STATUS=$V  低三位=0x$(printf %x $(( $V & 7 )))  KVOK=$(( ($V >> 3) & 1 ))" ;;
    esac
}
lo3() { V=$(busybox devmem $SY_STATUS 2>/dev/null || echo 0); echo $(( $V & 7 )); }

stop_daemon() {
    for p in $(pids); do kill $p 2>/dev/null; done
    sleep 2
    for p in $(pids); do kill -9 $p 2>/dev/null; done
    sleep 1
    rm -f /tmp/pqchsm_fpgad.sock 2>/dev/null
    n=$(pids | wc -l)
    if [ "$n" != "0" ]; then
        echo "  ✗ 还有 $n 个 daemon 没停掉：$(pids | tr '\n' ' ')" | tee -a $OUT
        fail=1
    fi
}

echo "载入演示位流 zu3eg_hsm_dev.bit" | tee -a $OUT
mkdir -p /lib/firmware
cp $D/zu3eg_hsm_dev.bit /lib/firmware/ 2>/dev/null
fpgautil -b /lib/firmware/zu3eg_hsm_dev.bit -f Full >> $OUT 2>&1
sleep 2
ST=$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)
echo "fpga_manager state = $ST" | tee -a $OUT
case "$ST" in operating) ;; *) echo "PL 不是 operating，停" | tee -a $OUT; exit 2 ;; esac
echo "位流刚重配，PL 是干净的：$(kv)" | tee -a $OUT

run_round() {   # $1 标签  $2 daemon 路径
    echo "" | tee -a $OUT
    echo "===== $1：$2 =====" | tee -a $OUT
    stop_daemon
    busybox devmem $SY_CTRL 32 0x1 2>/dev/null    # 手工擦一次，两轮之间隔离
    sleep 1
    ZEROED=$(lo3)                                  # ← 标定："刚擦过"长这样
    echo "  起点（刚手工 ZEROIZE 过）  $(kv)" | tee -a $OUT

    setsid $2 -lock >> $D/hsm-daemon.log 2>&1 < /dev/null &
    sleep 5
    RUN=$(pids | tr '\n' ' ')
    echo "  在跑的 daemon PID：[$RUN]" | tee -a $OUT
    # 闸门：9797 必须归本轮这个二进制。对不上就退出，不产出结果。
    OWNER=$(ps | grep "$(basename $2)" | grep -v grep | busybox awk '{print $1}' | head -1)
    if [ -z "$OWNER" ]; then
        echo "  ✗ $2 没起来（9797 大概还被别人占着）—— 本轮作废" | tee -a $OUT
        fail=1
        return
    fi
    echo "  ✓ 本轮由 PID ${OWNER}（$(basename $2)）提供服务" | tee -a $OUT

    $D/sdf_demo > $D/sdf_$1.txt 2>&1
    echo "  sdf_demo rc=$?  ✅ 计数 $(grep -c '✅' $D/sdf_$1.txt 2>/dev/null)" | tee -a $OUT
    sleep 2
    AFTER=$(lo3)
    echo "  会话结束后  $(kv)" | tee -a $OUT
    if [ "$AFTER" = "$ZEROED" ]; then
        echo "  → 低三位与本轮标定的「刚擦过」一致（0x$(printf %x $ZEROED)）：**发过 SY_CTRL**" | tee -a $OUT
        eval "R_$1=zeroed"
    else
        echo "  → 低三位 0x$(printf %x $AFTER) ≠ 标定 0x$(printf %x $ZEROED)：**没发 SY_CTRL**" | tee -a $OUT
        eval "R_$1=dirty"
    fi
}

run_round A_old $D/pqchsm_fpgad.pre_b1
run_round B_new $D/pqchsm_fpgad.b1new

echo "" | tee -a $OUT
echo "===== 判据 =====" | tee -a $OUT
echo "  A_old = $R_A_old   （期望 dirty —— Z-05 那个缺陷）" | tee -a $OUT
echo "  B_new = $R_B_new   （期望 zeroed —— 已修）" | tee -a $OUT
if [ "$R_A_old" = "dirty" ] && [ "$R_B_new" = "zeroed" ]; then
    echo "  ✓ 第 3 项成立：新 daemon 会话结束时确实发了 SY_CTRL.ZEROIZE" | tee -a $OUT
else
    echo "  ✗ 第 3 项不成立" | tee -a $OUT
    fail=1
fi

# ---- 收尾：把新 daemon 正式装成 pqchsm_fpgad 并起来 ----
stop_daemon
cp -f $D/pqchsm_fpgad.b1new $D/pqchsm_fpgad
chmod +x $D/pqchsm_fpgad
sync
setsid $D/pqchsm_fpgad -lock >> $D/hsm-daemon.log 2>&1 < /dev/null &
sleep 5
echo "" | tee -a $OUT
echo "收尾：正式装上新 daemon，PID [$(pids | tr '\n' ' ')]" | tee -a $OUT
md5sum $D/pqchsm_fpgad $D/pqchsm_fpgad.b1new | tee -a $OUT
echo "fail=$fail" | tee -a $OUT
sync
