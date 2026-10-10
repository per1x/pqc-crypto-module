#!/usr/bin/env bash
# http_demo.sh —— 从另一台机器经 HTTPS（JSON）调用密码机，按业务流程走一遍
#
#     ./demo/remote/http_demo.sh <板子IP> [凭据目录]
#
# 凭据目录默认 ~/.config/pqchsm/pki，里面要有 hsm_ca.crt / client.crt / client.key
# （与远程 SDF 口同一套，见 tools/demo_remote.sh --provision）。
# 依赖：curl、python3（只用来排版 JSON）。
#
# 每一步：打出 curl 命令 → 打出 HTTP 状态与响应 JSON → 打出这次请求在密码机里
# 各层执行了什么（响应里的 trace 字段：TEE / TEE→PL / PL / NW）。
# 输出格式与板上的业务演示相同，可以直接交给 tools/demo_to_html.py 生成网页。
set -u

HOST=${1:?用法：http_demo.sh <板子IP> [凭据目录]}
PKI=${2:-$HOME/.config/pqchsm/pki}
CN=axu3egb-hsm-01                       # 设备证书的 CN；curl 按它校验主机名
BASE="https://$CN:8443"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for f in hsm_ca.crt client.crt client.key; do
  [ -r "$PKI/$f" ] || { echo "缺 $PKI/$f（先跑 tools/demo_remote.sh <IP> --provision）"; exit 1; }
done

# ⚠️ --noproxy：开着系统代理（HTTPS_PROXY）时 curl 会把请求交给代理，
#    表现是"连接超时"，与密码机无关。--resolve 让 URL 里用证书上的名字。
CURL=(curl -sS --noproxy '*' --max-time 30 --cacert "$PKI/hsm_ca.crt"
      --cert "$PKI/client.crt" --key "$PKI/client.key"
      --resolve "$CN:8443:$HOST")

B=$'\033[1m'; D=$'\033[2m'; C=$'\033[36m'; N=$'\033[0m'
G=$'\033[32m'; R=$'\033[31m'; Y=$'\033[33m'

part() { echo; echo "${B}══════ $* ══════${N}"; }
step() { echo; echo "${B}▶ $*${N}"; }
note() { echo "  ${D}$*${N}"; }

FMT=$(cat <<'PY'
import json, sys
code, path = sys.argv[1], sys.argv[2]
G, Y, C, D, N = "\033[32m", "\033[33m", "\033[36m", "\033[2m", "\033[0m"
try:
    j = json.load(open(path))
except Exception:
    print("  HTTP " + code + "（不是 JSON）"); sys.exit(0)
trace = j.pop("trace", None); j.pop("request", None); j.pop("client_cn", None)
def short(v):
    if isinstance(v, str) and len(v) > 72:
        return v[:64] + "…（共 %d 字符）" % len(v)
    if isinstance(v, list):
        return [ {k: short(x) for k, x in i.items()} if isinstance(i, dict) else short(i) for i in v ]
    return v
print("  HTTP " + code)
for line in json.dumps({k: short(v) for k, v in j.items()}, ensure_ascii=False, indent=2).splitlines():
    print("  " + line)
for t in trace or []:
    tag = "[" + t["layer"] + "]"
    col = {"TEE": G, "TEE→PL": Y, "PL": C}.get(t["layer"], D)
    print("  " + col + "│ " + tag.ljust(9) + " " + t["detail"] + N)
PY
)

# call METHOD PATH [JSON]  —— 响应留在 $WORK/last.json
call() {
  local m=$1 p=$2 body=${3:-}
  local shown="curl -X $m https://<板子>:8443$p"
  # 显示时把长 base64 缩成占位符（实际发出去的请求不变）
  [ -n "$body" ] && shown="$shown -d '$(python3 -c '
import re, sys
print(re.sub(r"\"([A-Za-z0-9+/=]{80,})\"", lambda m: "\"<base64，共 %d 字符>\"" % len(m.group(1)), sys.argv[1]))
' "$body")'"
  echo "${C}\$ ${shown}${N}"
  local args=("${CURL[@]}" -X "$m" -o "$WORK/last.json" -w '%{http_code}')
  [ -n "$body" ] && args+=(-H 'Content-Type: application/json' -d "$body")
  local code
  code=$("${args[@]}" "$BASE$p$( [[ $p == *\?* ]] && echo '&' || echo '?')trace=1" 2>"$WORK/err") \
    || { echo "  ${R}curl 失败：$(cat "$WORK/err")${N}"; return 1; }
  python3 -c "$FMT" "$code" "$WORK/last.json"
}

get() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$WORK/last.json" "$1"; }

echo
echo "${B}  AXU3EGB 后量子密码机 —— HTTP 接口调用演示${N}"
echo "  上层应用经 HTTPS（双向证书）调用 · 密钥在 TEE 里管 · 运算在 FPGA 里做"
echo
echo "  日志标签：${G}[TEE]${N} TA 里做的   ${Y}[TEE→PL]${N} 安全通路写进 FPGA   ${C}[PL]${N} FPGA 里算的   ${D}[NW]${N} daemon 搬密文"

# 清掉上次没删干净的演示密钥（静默）
for k in http-data http-signer http-kem; do
  "${CURL[@]}" -o /dev/null -X DELETE "$BASE/v1/keys/$k" 2>/dev/null || true
done

# ------------------------------------------------------------------ 〇
part "HTTP · 〇 连接与身份"
step "带客户端证书：拿到设备信息"
call GET /v1/info
step "不带客户端证书：TLS 握手阶段就被拒"
echo "${C}\$ curl https://<板子>:8443/v1/info   # 不带 --cert / --key${N}"
out=$(curl -sS --noproxy '*' --max-time 15 --cacert "$PKI/hsm_ca.crt" --resolve "$CN:8443:$HOST" \
      "$BASE/v1/info" 2>&1)
if [[ $out == *"SSL"* || $out == *"ssl"* || $out == *"handshake"* ]]; then
  echo "  ✗ TLS 握手被拒：服务端要求客户端证书（curl 报：${out:0:60}…）"
else
  echo "  ✗ 被拒：${out:0:110}"
fi
note "网关只有 HTTPS + 双向证书，没有明文 HTTP；凭据不全时它干脆不监听。"

# ------------------------------------------------------------------ 一
part "HTTP · 一 密钥管理"
step "生成 SM4 数据密钥"
call POST /v1/keys '{"name":"http-data","alg":"sm4"}'
step "生成 ML-DSA-65 签名密钥"
call POST /v1/keys '{"name":"http-signer","alg":"mldsa65"}'
step "生成 ML-KEM-768 密钥封装密钥"
call POST /v1/keys '{"name":"http-kem","alg":"mlkem768"}'
step "列出密钥"
call GET /v1/keys
step "查看一把密钥"
call GET /v1/keys/http-data
note "网关保存的只是公钥和 TA 包裹的密文 blob（blob_head_hex 以 \"PWRP\" 开头）。"

# ------------------------------------------------------------------ 二
part "HTTP · 二 数据加解密（SM4-CBC）"
PT='采购合同 PO-2026-1011，金额：人民币 100,000.00 元'
step "加密"
call POST /v1/keys/http-data/encrypt "{\"plaintext\":\"$PT\"}"
CT=$(get ciphertext_b64)
step "解密"
call POST /v1/keys/http-data/decrypt "{\"ciphertext_b64\":\"$CT\"}"
[ "$(get plaintext)" = "$PT" ] && echo "  ${G}✓ 解密结果与原文一致${N}" || echo "  ${R}✗ 解密结果与原文不一致${N}"

# ------------------------------------------------------------------ 三
part "HTTP · 三 签名与验签（ML-DSA-65）"
MSG='合同 PO-2026-1011：金额 100,000.00 元，交付 2026-12-31'
step "签名"
call POST /v1/keys/http-signer/sign "{\"message\":\"$MSG\"}"
SIG=$(get signature_b64)
step "验签"
call POST /v1/keys/http-signer/verify "{\"message\":\"$MSG\",\"signature_b64\":\"$SIG\"}"
step "金额被改成 900,000 再验"
call POST /v1/keys/http-signer/verify "{\"message\":\"${MSG/100,000/900,000}\",\"signature_b64\":\"$SIG\"}"
[ "$(get valid)" = "False" ] && echo "  ${G}✓ 被改过的消息验不过${N}"

# ------------------------------------------------------------------ 四
part "HTTP · 四 密钥封装（ML-KEM-768）"
step "发送方：用公钥封装"
call POST /v1/keys/http-kem/encapsulate '{}'
KCT=$(get ciphertext_b64); SS1=$(get shared_secret_hex)
step "接收方：用私钥解封装"
call POST /v1/keys/http-kem/decapsulate "{\"ciphertext_b64\":\"$KCT\"}"
[ "$(get shared_secret_hex)" = "$SS1" ] && echo "  ${G}✓ 双方共享密钥一致${N}" || echo "  ${R}✗ 共享密钥不一致${N}"

# ------------------------------------------------------------------ 五
part "HTTP · 五 杂凑与随机数"
call POST /v1/sm3 '{"data":"abc"}'
[ "$(get sm3_hex)" = 66c7f0f462eeedd9d1f2d46bdc10e4e24167c4875cf2f7a2297da02b8f4ba8e0 ] \
  && echo "  ${G}✓ SM3(\"abc\") 对上 GB/T 32905 附录 A${N}"
call POST /v1/random '{"length":32}'

# ------------------------------------------------------------------ 六
part "HTTP · 六 销毁密钥"
step "销毁签名密钥，之后再用它签名"
call DELETE /v1/keys/http-signer
call POST /v1/keys/http-signer/sign "{\"message\":\"$MSG\"}"
step "清理"
call DELETE /v1/keys/http-data
call DELETE /v1/keys/http-kem

echo
echo "${G}${B}HTTP 演示结束。${N}"
