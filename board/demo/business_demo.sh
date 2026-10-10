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
# 第八部分经本机 HTTP 口（127.0.0.1:8080，hsm_httpd）调用，用的是网关自己的密钥库，
# 演示的几把密钥最后都会删掉。
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

# 本机 HTTP 调用的小工具：http 方法 路径 [JSON]
# 板上没有 curl，busybox wget 又发不了 POST —— 就用 nc 把一个原样的 HTTP/1.1 请求
# 送到 127.0.0.1:8080。打出状态行和排好版的响应 JSON（长 base64 / 十六进制缩写显示，
# 完整响应留在 $WORK/last.json，后面的步骤从里面取值）。
cat > "$WORK/http" <<'EOF'
#!/bin/sh
m=$1; p=$2; b=${3:-}
n=$(printf %s "$b" | wc -c)
{ printf '%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n' "$m" "$p" "$n"
  printf %s "$b"; } | nc 127.0.0.1 8080 | tr -d '\r' > "$(dirname "$0")/last.raw"
sed -n '1s/^HTTP\/1\.1 \([0-9]*\).*/HTTP \1/p' "$(dirname "$0")/last.raw"
sed '1,/^$/d' "$(dirname "$0")/last.raw" > "$(dirname "$0")/last.json"
awk '
function out(s) { printf "%s", s }
function nl() { out("\n"); for (k = 0; k < ind; k++) out("  ") }
{ t = t $0 }
END {
  ind = 0; n = length(t); i = 1
  while (i <= n) {
    c = substr(t, i, 1)
    if (c == "\"") {
      j = i + 1
      while (j <= n && substr(t, j, 1) != "\"") { if (substr(t, j, 1) == "\\") j++; j++ }
      str = substr(t, i + 1, j - i - 1)
      if (length(str) > 72 && str ~ /^[A-Za-z0-9+\/=]+$/)
        str = substr(str, 1, 48) "…（共 " length(str) " 字符）"
      out("\"" str "\""); i = j + 1; continue
    }
    if (c == "{" || c == "[") {
      if (substr(t, i + 1, 1) == (c == "{" ? "}" : "]")) { out(c (c == "{" ? "}" : "]")); i += 2; continue }
      out(c); ind++; nl()
    } else if (c == "}" || c == "]") { ind--; nl(); out(c) }
    else if (c == ",") { out(","); nl() }
    else if (c == ":") out(": ")
    else if (c != " " && c != "\n") out(c)
    i++
  }
  out("\n")
}' "$(dirname "$0")/last.json"
EOF
chmod +x "$WORK/http"
# 从上一次 HTTP 响应里取一个字符串 / 布尔字段
jv() {
    v=$(sed -n 's/.*"'"$1"'": *"\([^"]*\)".*/\1/p' "$WORK/last.json")
    [ -n "$v" ] || v=$(sed -n 's/.*"'"$1"'": *\([^",}]*\).*/\1/p' "$WORK/last.json")
    echo "$v"
}
# 显示命令时把长 base64 缩成占位符（实际执行的命令不变）
short() { awk '{ while (match($0, /[A-Za-z0-9+\/=]{80,}/)) $0 = substr($0, 1, RSTART - 1) "<base64，共 " RLENGTH " 字符>" substr($0, RSTART + RLENGTH); print }'; }

part() { echo; echo "${B}══════ $* ══════${N}"; }
step() { echo; echo "${B}▶ $*${N}"; }
note() { echo "  ${D}$*${N}"; }

# 跑一条命令：打命令 → 打输出 → 打这条命令产生的分层日志
run() {
    mark=$(wc -l < "$LOG")
    echo "${C}\$ $(printf '%s\n' "$*" | short)${N}"
    sh -c "$*" 2>&1 | sed 's/^/  /'
    # 会话结束那一行是客户端断开之后才写的，稍等一下再取日志
    usleep 150000 2>/dev/null || sleep 1
    tail -n +"$((mark + 1))" "$LOG" \
      | grep -E "\[TEE\]|\[TEE→PL\]|\[PL\]|\[NW\]|\[HTTP\] CN=" \
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

# ------------------------------------------------------------------ 八
part "八 应用经 HTTP 接口调用（本机 127.0.0.1:8080）"
note "上层应用不链 libsdfe，发 JSON 就行。网关 hsm_httpd 与 hsmctl 共用同一套业务代码，"
note "密钥一样在 TEE 里管、运算一样在 FPGA 里做。板上没有 curl，这里用 nc 直接发原样 HTTP 请求："
run "sed -n 2,5p http"
for k in web-data web-signer web-kem; do ./http DELETE /v1/keys/$k >/dev/null 2>&1; done

step "设备信息"
run ./http GET /v1/info

step "生成密钥：SM4 数据密钥、ML-DSA-65 签名密钥、ML-KEM-768 封装密钥"
run "./http POST /v1/keys '{\"name\":\"web-data\",\"alg\":\"sm4\"}'"
run "./http POST /v1/keys '{\"name\":\"web-signer\",\"alg\":\"mldsa65\"}'"
run "./http POST /v1/keys '{\"name\":\"web-kem\",\"alg\":\"mlkem768\"}'"
run ./http GET /v1/keys

step "加密 / 解密"
PT='订单 SO-2026-1011，金额：人民币 52,800.00 元'
run "./http POST /v1/keys/web-data/encrypt '{\"plaintext\":\"$PT\"}'"
CT=$(jv ciphertext_b64)
run "./http POST /v1/keys/web-data/decrypt '{\"ciphertext_b64\":\"$CT\"}'"
[ "$(jv plaintext)" = "$PT" ] && echo "  ${G}✓ 解密结果与原文一致${N}" || echo "  ${R}✗ 解密结果与原文不一致${N}"

step "签名 / 验签 / 改过的消息验不过"
MSG='订单 SO-2026-1011：金额 52,800.00 元'
run "./http POST /v1/keys/web-signer/sign '{\"message\":\"$MSG\"}'"
SIG=$(jv signature_b64)
run "./http POST /v1/keys/web-signer/verify '{\"message\":\"$MSG\",\"signature_b64\":\"$SIG\"}'"
FORGED=$(echo "$MSG" | sed 's/52,800/92,800/')
run "./http POST /v1/keys/web-signer/verify '{\"message\":\"$FORGED\",\"signature_b64\":\"$SIG\"}'"
[ "$(jv valid)" = false ] && echo "  ${G}✓ 金额被改过的消息验不过${N}"

step "密钥封装 / 解封装"
run "./http POST /v1/keys/web-kem/encapsulate '{}'"
KCT=$(jv ciphertext_b64); SS1=$(jv shared_secret_hex)
run "./http POST /v1/keys/web-kem/decapsulate '{\"ciphertext_b64\":\"$KCT\"}'"
[ "$(jv shared_secret_hex)" = "$SS1" ] && echo "  ${G}✓ 双方共享密钥一致${N}" || echo "  ${R}✗ 共享密钥不一致${N}"

step "杂凑与随机数"
run "./http POST /v1/sm3 '{\"data\":\"abc\"}'"
run "./http POST /v1/random '{\"length\":32}'"

step "销毁签名密钥后再用它签名"
run ./http DELETE /v1/keys/web-signer
run "./http POST /v1/keys/web-signer/sign '{\"message\":\"$MSG\"}'"
note "这个口只绑在回环上，网线那头连不到；对外只有 8443（HTTPS + 双向证书）。"
for k in web-data web-kem; do ./http DELETE /v1/keys/$k >/dev/null 2>&1; done

echo
echo "${G}${B}演示结束。${N}  工作目录 $WORK 已删除。"
cd / && rm -rf "$WORK"
