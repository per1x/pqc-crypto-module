#!/usr/bin/env bash
# full_demo.sh —— 一条命令跑完整演示：板上命令行业务流程 + HTTPS 接口调用，并生成网页报告
#
#     ./demo/remote/full_demo.sh <板子IP> [凭据目录]
#
# 第一部分经 ssh 在板上跑 /media/sd-mmcblk1p2/demo/run.sh（hsmctl 命令行，看得见每一步
# 在 TEE / PL 哪里执行）；第二部分在本机用 curl 经 HTTPS（双向证书）调用同一台密码机。
# 两份输出都留在 demo-out/ 下，并合成一页 demo-out/demo.html。
set -u
HOST=${1:?用法：full_demo.sh <板子IP> [凭据目录]}
PKI=${2:-$HOME/.config/pqchsm/pki}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${DEMO_OUT:-$PWD/demo-out}
mkdir -p "$OUT"

echo "════════ 第一部分：板上命令行（ssh root@${HOST}） ════════"
ssh -o HostKeyAlgorithms=+ssh-rsa "root@$HOST" 'sh /media/sd-mmcblk1p2/demo/run.sh' | tee "$OUT/cli.txt"

echo
echo "════════ 第二部分：HTTPS 接口（本机 curl → $HOST:8443） ════════"
"$ROOT/demo/remote/http_demo.sh" "$HOST" "$PKI" | tee "$OUT/http.txt"

python3 "$ROOT/tools/demo_to_html.py" "$OUT/cli.txt" "$OUT/http.txt" "$OUT/demo.html"
echo "网页报告：$OUT/demo.html"
