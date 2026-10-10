#!/bin/sh
# business_demo.sh —— 按业务流程演示密码机（部署为 /media/sd-mmcblk1p2/demo/run.sh）
#
#     ssh root@<板子IP>
#     sh /media/sd-mmcblk1p2/demo/run.sh
#
# 每一步：先打出要执行的命令，再打出它的输出，最后摘出 daemon 日志里**这一条命令**
# 产生的分层记录 —— 看得见这一步是在 TEE 里做的还是在 FPGA（PL）里做的：
#
#   [TEE]     OP-TEE 里的 TA：生成 / 包裹 / 解开密钥
#   [TEE→PL]  TA 经安全世界把密钥材料写进 FPGA（普通世界不经手）
#   [PL]      FPGA 里的密码核做运算（带时延）
#   [NW]      普通世界 daemon：只搬密文 blob
#
# 这些日志行只在对应那一层的调用**真的成功返回之后**才写，是执行记录，不是旁白。
#
# 演示用的密钥库在 /tmp 下，跑完即删；不改启动配置、不换位流。
set -u

HSM=/media/sd-mmcblk1p2/hsm
LOG=$HSM/hsm-boot.log
WORK=/tmp/hsm-demo
export HSM_KEYSTORE=$WORK/keystore
PATH=$HSM:$PATH

B=$(printf '\033[1m'); D=$(printf '\033[2m'); C=$(printf '\033[36m'); N=$(printf '\033[0m')
G=$(printf '\033[32m'); R=$(printf '\033[31m'); Y=$(printf '\033[33m')

rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK" || exit 1

# 改一个字节的小工具。⚠️ 这块板的 busybox dd **没有 conv=notrunc**：第一版用
# `printf Z | dd … conv=notrunc 2>/dev/null` 改字节，dd 直接报错退出、文件原封不动，
# 而错误被吞了 —— 于是"篡改后被拒"那两步实际上什么都没篡改，还显示成"通过"。
# 现在用 dd 读出前后两段再拼回去（这块板的 busybox 也没有 head -c），并且**核对确实改了**，没改成就当场停。
cat > "$WORK/flip.sh" <<'EOF'
f=$1; off=$2
old=$(dd if="$f" bs=1 skip="$off" count=1 2>/dev/null | hexdump -v -e '1/1 "%02x"')
new=5a; [ "$old" = 5a ] && new=59
sz=$(wc -c < "$f")
{ dd if="$f" bs=1 count="$off" 2>/dev/null; printf "\x$new"; dd if="$f" bs=1 skip="$((off + 1))" 2>/dev/null; } > "$f.flip" && mv "$f.flip" "$f"
got=$(dd if="$f" bs=1 skip="$off" count=1 2>/dev/null | hexdump -v -e '1/1 "%02x"')
[ "$got" = "$new" ] && [ "$(wc -c < "$f")" = "$sz" ] || { echo "改字节失败"; exit 1; }
echo "已把 $(basename "$f") 第 $off 字节从 0x${old} 改成 0x${new}（文件长度不变：$sz 字节）"
EOF

part() { echo; echo "${B}══════ $* ══════${N}"; }
step() { echo; echo "${B}▶ $*${N}"; }
note() { echo "  ${D}$*${N}"; }

# 跑一条命令：打命令 → 打输出 → 打这条命令产生的分层日志
run() {
    mark=$(wc -l < "$LOG")
    echo "${C}\$ $*${N}"
    sh -c "$*" 2>&1 | sed 's/^/  /'
    # 会话结束那一行是客户端断开之后才写的，稍等一下再取日志
    usleep 150000 2>/dev/null || sleep 1
    tail -n +"$((mark + 1))" "$LOG" \
      | grep -E "\[TEE\]|\[TEE→PL\]|\[PL\]|\[NW\]" \
      | sed -E 's/^\[[0-9:]+\] //' \
      | while IFS= read -r l; do
            case "$l" in
            "[TEE→PL]"*) echo "  ${Y}│ $l${N}" ;;
            "[TEE]"*)    echo "  ${G}│ $l${N}" ;;
            "[PL]"*)     echo "  ${C}│ $l${N}" ;;
            *)           echo "  ${D}│ $l${N}" ;;
            esac
        done
}

echo
echo "${B}  AXU3EGB 后量子密码机 —— 业务流程演示${N}"
echo "  密钥在 TEE 里管 · 运算在 FPGA 里做 · 应用只调接口"
echo
echo "  日志标签：${G}[TEE]${N} TA 里做的   ${Y}[TEE→PL]${N} 安全通路写进 FPGA   ${C}[PL]${N} FPGA 里算的   ${D}[NW]${N} daemon 搬密文"

grep -q "READY=yes" "$HSM/HSM_STATUS" 2>/dev/null || { echo "${R}密码机没就绪（看 $HSM/HSM_STATUS）${N}"; exit 1; }

# ------------------------------------------------------------------ 〇
part "〇 设备"
run hsmctl info

# ------------------------------------------------------------------ 一
part "一 密钥管理"
step "生成一把 SM4 数据密钥"
run hsmctl keygen sm4 data-key
note "密钥由 TA 生成（熵来自 OP-TEE PRNG 与 FPGA 里的真随机数），应用拿到的只是包裹后的 blob。"

step "生成一对 ML-DSA-65 签名密钥"
run hsmctl keygen mldsa65 signer

step "生成一对 ML-KEM-768 密钥封装密钥"
run hsmctl keygen mlkem768 kem-key

step "查看密钥库"
run hsmctl list
run hsmctl show data-key
note "密钥文件里只有公钥和密文 blob。拷到别处也没用 —— 只有这块板上的 TA 解得开。"

# ------------------------------------------------------------------ 二
part "二 数据加解密（SM4-CBC）"
step "准备一份业务数据"
run "printf '采购合同 PO-2026-1010\n甲方：示例公司\n金额：人民币 100,000.00 元\n交付：2026-12-31\n' > contract.txt; cat contract.txt"

step "加密"
run hsmctl encrypt data-key contract.txt contract.enc
run "hexdump -C contract.enc | head -4"
note "每次加密：TA 解开 blob、经安全通路把密钥装进 FPGA；之后每个分组都在 FPGA 里算，不再进 TEE。"

step "解密并比对"
run hsmctl decrypt data-key contract.enc contract.dec
run "cmp contract.txt contract.dec && echo '解密结果与原文逐字节一致'"

# ------------------------------------------------------------------ 三
part "三 签名与验签（ML-DSA-65）"
step "签署合同"
run hsmctl sign signer contract.txt contract.sig

step "验签"
run hsmctl verify signer contract.txt contract.sig

step "有人把金额改了一位，再验"
run "sed 's/100,000/900,000/' contract.txt > contract.forged; grep 金额 contract.forged"
run hsmctl verify signer contract.forged contract.sig

# ------------------------------------------------------------------ 四
part "四 密钥封装 / 密钥协商（ML-KEM-768）"
step "发送方：只用对方公钥封装出一个共享密钥"
run hsmctl encap kem-key kem.ct
step "接收方：用私钥解封装，得到同一个共享密钥"
run hsmctl decap kem-key kem.ct
note "双方的共享密钥一致 —— 可以拿它做会话密钥。私钥全程没离开 TEE / FPGA。"

# ------------------------------------------------------------------ 五
part "五 杂凑与随机数"
run hsmctl hash contract.txt
run hsmctl random 32

# ------------------------------------------------------------------ 六
part "六 备份与恢复"
step "备份整个密钥库"
run hsmctl backup keys.pqb
note "备份包里全是 TA 包裹的密文，每项带 SM3 校验（SM3 在 FPGA 里算）。"

step "签名密钥丢了（销毁）"
run hsmctl destroy signer
run hsmctl sign signer contract.txt contract2.sig

step "从备份恢复，原来的签名仍能验过 —— 是同一把密钥"
run hsmctl restore keys.pqb
run hsmctl verify signer contract.txt contract.sig

step "备份包被人改了一个字节"
run "cp keys.pqb keys.bad; sh flip.sh keys.bad \$((\$(wc -c < keys.bad) - 40))"
run hsmctl restore keys.bad

# ------------------------------------------------------------------ 七
part "七 安全性检查"
step "篡改密钥文件里的 blob，TA 拒绝装载"
run "cp \$HSM_KEYSTORE/data-key.key data-key.bak; sh flip.sh \$HSM_KEYSTORE/data-key.key \$((\$(wc -c < data-key.bak) - 1))"
run hsmctl encrypt data-key contract.txt x.enc
run "cp data-key.bak \$HSM_KEYSTORE/data-key.key; echo '已还原'"

step "绕过 TEE：普通世界直接往 FPGA 的密钥装载口写一把密钥"
run d7probe
note "FPGA 的装载口只认安全世界的事务；普通世界写进去的 11 笔全部被丢弃并计数。"

step "所有会话都已结束：FPGA 里的对称金库是空的"
run "d7probe vmap"

echo
echo "${G}${B}演示结束。${N}  工作目录 $WORK 已删除。"
cd / && rm -rf "$WORK"
