#!/bin/sh
# demo_remote.sh —— 在本机（Mac / 任意主机）调用密码机。**主路径不用 SSH。**
#
#   ./tools/demo_remote.sh --fetch-token   # 只需做一次：把一次性口令存到本地
#   ./tools/demo_remote.sh                 # 之后就是纯本地：编译 + TCP 调用
#   ./tools/demo_remote.sh --smoke         # 最小冒烟，只打关键结论
#   ./tools/demo_remote.sh --status        # 只连一下看设备在不在
#
# 可用环境变量覆盖（正常情况下一个都不用设）：
#   BOARD=192.168.50.175        密码机地址
#   PORT=9797                   daemon 的 TCP 口
#   HSM_TOKEN=<口令>            直接给口令，优先级最高
#   HSM_TOKEN_FILE=<路径>       口令文件（默认 ~/.config/pqchsm/token）
#   SSH_KEY=~/.ssh/id_rsa       只有 --fetch-token 会用到
#
# ============================================================================
# 【调用链 —— 密码运算一步都不经过 SSH】
# ============================================================================
#   本机 sdf_demo（原生二进制）
#     └─ libsdfe ──── TCP 9797 ───▶ 板上 pqchsm_fpgad
#                                     └─ /dev/secmmio
#                                          └─ EL3 SiP（安全世界）
#                                               └─ FPGA 里的密码核
#
# `sdf_demo` **只链接 libsdfe，完全不链接任何密码库**（纯 socket，不依赖
# OpenSSL / liboqs），所以它自己算不出任何东西 —— 它打印出的每一个对得上标准向量
# 的数值都只能来自 FPGA。这是刻意的设计，也是演示时最该指出的一点。
#
# SSH 只在 `--fetch-token` 里出现一次，用途仅仅是 `cat` 一个文本文件。
# 口令一旦存到本地，**之后的调用不需要板子的 shell、也不需要任何凭据**。
#
# ⚠️ 真实部署里这个口令应当带外分发，而不是每次 ssh 去 cat。`--fetch-token`
#    只是给手上就有板子 shell 的人省事。
set -eu

BOARD=${BOARD:-192.168.50.175}
PORT=${PORT:-9797}
TOKEN_FILE=${HSM_TOKEN_FILE:-$HOME/.config/pqchsm/token}
SSH_KEY=${SSH_KEY:-$HOME/.ssh/id_rsa}

MODE=full
case "${1:-}" in
	--smoke)       MODE=smoke ;;
	--status)      MODE=status ;;
	--fetch-token) MODE=fetch ;;
	-h|--help)     sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	"") ;;
	*) echo "未知参数：$1（用 --help 看用法）" >&2; exit 2 ;;
esac

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CLIENT=$ROOT/build-demo/sdf_demo

ok()   { printf '  \033[32m✓\033[0m %s\n' "$*"; }
step() { printf '\n\033[1m%s\033[0m\n' "$*"; }
die()  { printf '  \033[31m✗\033[0m %s\n' "$1" >&2; shift
         for l in "$@"; do printf '    %s\n' "$l" >&2; done; exit 1; }

# ---- --fetch-token：唯一会用 SSH 的动作，一次性 ----------------------------
if [ "$MODE" = fetch ]; then
	step "取一次性口令（这是本脚本唯一用 SSH 的地方）"
	# 板子跑 dropbear 2019.78 只认 RSA；新版 OpenSSH 默认**不再发** RSA(SHA-1)
	# 签名，所以除了 HostKeyAlgorithms 还必须放开 PubkeyAccepted*。
	# 该选项在 OpenSSH < 8.5 上叫 PubkeyAcceptedKeyTypes —— 用 ssh -G 探测认哪个。
	if ssh -G -o PubkeyAcceptedAlgorithms=+ssh-rsa localhost >/dev/null 2>&1; then
		PK="-o PubkeyAcceptedAlgorithms=+ssh-rsa"
	else
		PK="-o PubkeyAcceptedKeyTypes=+ssh-rsa"
	fi
	SO="-o HostKeyAlgorithms=+ssh-rsa $PK -o ConnectTimeout=8 -o StrictHostKeyChecking=no"
	[ -f "$SSH_KEY" ] && SO="$SO -i $SSH_KEY"

	# shellcheck disable=SC2086
	TOK=$(ssh $SO "root@$BOARD" 'cat /media/sd-mmcblk1p2/hsm/hsm_token' 2>/dev/null || true)
	[ -n "$TOK" ] || die "没取到口令" \
		"· 若报 Permission denied (publickey,password)：这**不是**公钥没装，" \
		"  是本机 OpenSSH 拒绝发 RSA(SHA-1) 签名。脚本已自动加 ${PK}；" \
		"  仍失败就换钥匙： SSH_KEY=~/.ssh/别的私钥 $0 --fetch-token" \
		"· 板上没有 hsm_token 文件时 daemon 是不监听的（fail-closed）" \
		"· 也可以手抄： HSM_TOKEN=<口令> $0"

	mkdir -p "$(dirname "$TOKEN_FILE")"
	umask 077
	printf '%s\n' "$TOK" > "$TOKEN_FILE"
	chmod 600 "$TOKEN_FILE"
	ok "已存到 ${TOKEN_FILE}（0600，${#TOK} 字符）"
	printf '\n以后直接跑 %s —— 不再需要 SSH。\n' "$0"
	exit 0
fi

# ---- 1. 口令：三个来源，都不用 SSH -----------------------------------------
if [ -n "${HSM_TOKEN:-}" ]; then
	TOK=$HSM_TOKEN; TOKSRC="环境变量 HSM_TOKEN"
elif [ -f "$TOKEN_FILE" ]; then
	TOK=$(cat "$TOKEN_FILE"); TOKSRC=$TOKEN_FILE
else
	die "本地没有口令，也没给 HSM_TOKEN" \
	    "取一次就行（唯一用 SSH 的动作）：" \
	    "    $0 --fetch-token" \
	    "或者手上已经有口令：" \
	    "    HSM_TOKEN=<口令> $0"
fi
[ -n "$TOK" ] || die "口令文件是空的：$TOKEN_FILE" "重新取： $0 --fetch-token"

# ---- 2. 客户端：仓库里没有 CMake 目标，两个文件直接编 ----------------------
SRC1=$ROOT/service/sdf_demo.c
SRC2=$ROOT/service/libsdfe.c
[ -f "$SRC1" ] || die "找不到 ${SRC1}（这个脚本要在仓库里跑）"

if [ ! -x "$CLIENT" ] || [ "$SRC1" -nt "$CLIENT" ] || [ "$SRC2" -nt "$CLIENT" ]; then
	command -v cc >/dev/null 2>&1 || die "没有 cc" "· macOS： xcode-select --install"
	mkdir -p "$(dirname "$CLIENT")"
	cc -O2 -Wall -Wextra -I"$ROOT/service" -o "$CLIENT" "$SRC1" "$SRC2" || die "编译失败"
	BUILT="刚编译"
else
	BUILT="已是最新"
fi

# ---- 3. 跑（纯 TCP，无 SSH）------------------------------------------------
step "本机调用密码机　$BOARD:$PORT"
ok "客户端　${BUILT}（${CLIENT}）"
ok "口令来自　$TOKSRC"
printf '\n'

run() { "$CLIENT" "$BOARD" "$TOK" "$PORT" 2>&1; }

# sdf_demo 自身对"连不上/口令不对"都会打明确的话，所以不另做探测；
# 只在它没走到最后一节时，把可执行的下一步补上。
diagnose() {
	printf '%s\n' "$1" | grep -q '连不上' && die "连不上 $BOARD:$PORT" \
		"· 板子可能还没起来（上电到就绪约 35 秒）" \
		"· macOS 的 ARP 缓存常是陈的： sudo arp -d $BOARD" \
		"· 端口不对就设 PORT=…"
	printf '%s\n' "$1" | grep -q '口令不对' && die "口令不对" \
		"· 板子重装过 token 就要重取： $0 --fetch-token"
	die "演示没跑到最后一节"
}

case "$MODE" in
status)
	OUT=$(run) || true
	printf '%s\n' "$OUT" | grep -E '^\[连接\]|^\[设备\]' || diagnose "$OUT"
	printf '%s\n' "$OUT" | grep -q '^\[设备\]' || diagnose "$OUT"
	printf '\n'; ok "设备在线（三个 VERSION 都应是 0x00010000）"
	;;
smoke)
	OUT=$(run) || true
	printf '%s\n' "$OUT" | grep -E '\[设备\]|共享密钥一致|GB/T|ML-DSA|旧句柄|全部完成' || true
	printf '%s\n' "$OUT" | grep -q '全部完成' || diagnose "$OUT"
	printf '\n'; ok "冒烟通过"
	;;
full)
	OUT=$(run) || true
	printf '%s\n' "$OUT"
	printf '%s\n' "$OUT" | grep -q '全部完成' || diagnose "$OUT"
	printf '\n'
	printf '───────────────────────────────────────────────\n'
	printf '演示时值得指出的两点：\n'
	printf '  · 三个 VERSION 都是 0x00010000 —— 密码核真的在 PL 里活着；\n'
	printf '    读到 0 就是被防火墙拒了，或者位流没载。\n'
	printf '  · 最后一节是**反证**：断开重连后旧句柄必须失效，\n'
	printf '    证明句柄是硬件侧的，不是应用自己编的。\n'
	;;
esac
