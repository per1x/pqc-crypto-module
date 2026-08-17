#!/bin/sh
# demo_remote.sh —— 一键从 Mac（或任意主机）验证 / 演示这台密码机。
#
#   ./tools/demo_remote.sh            # 完整九节演示
#   ./tools/demo_remote.sh --smoke    # 最小冒烟：只打关键结论
#   ./tools/demo_remote.sh --status   # 只看板子状态，不跑演示
#
# 可用环境变量覆盖（都有默认值，正常情况下一个都不用设）：
#   BOARD=192.168.50.175   板子 eth1 地址
#   PORT=9797              daemon 的 TCP 口
#   SSH_KEY=~/.ssh/id_rsa  用哪把私钥（留空则让 ssh 自己挑）
#
# ============================================================================
# 【这个脚本为什么存在】
# ============================================================================
# 照 docs/USAGE 敲会卡在两个地方，两个都不是"配置错了"，而是**看起来像别的问题**：
#
#   ① 从 Mac 直连板子会得到 `Permission denied (publickey,password)`。
#      看着像公钥没装 —— 其实公钥在。板子跑 dropbear 2019.78 只认 RSA，而新版
#      OpenSSH 默认**不再发** RSA(SHA-1) 签名。除了众所周知的
#      `HostKeyAlgorithms=+ssh-rsa`，还必须加 `PubkeyAcceptedAlgorithms=+ssh-rsa`
#      （OpenSSH < 8.5 上这个选项叫 `PubkeyAcceptedKeyTypes`，本脚本自动选）。
#
#   ② `sdf_demo` **没有 CMake 目标**，`cmake --build` 不会生成它。
#      它只链接 libsdfe（纯 socket，不依赖 OpenSSL / liboqs），两个文件直接编。
#
# 本脚本把这两条都处理掉，并且**每一步失败都给出可执行的下一步**，而不是只回一个
# 非零退出码。
set -eu

BOARD=${BOARD:-192.168.50.175}
PORT=${PORT:-9797}
SSH_KEY=${SSH_KEY:-$HOME/.ssh/id_rsa}

MODE=full
case "${1:-}" in
	--smoke)  MODE=smoke ;;
	--status) MODE=status ;;
	-h|--help)
		sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
		exit 0 ;;
	"") ;;
	*) echo "未知参数：$1（用 --help 看用法）" >&2; exit 2 ;;
esac

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CLIENT=$ROOT/build-demo/sdf_demo

say()  { printf '%s\n' "$*"; }
ok()   { printf '  \033[32m✓\033[0m %s\n' "$*"; }
bad()  { printf '  \033[31m✗\033[0m %s\n' "$*" >&2; }
step() { printf '\n\033[1m%s\033[0m\n' "$*"; }

die() { bad "$1"; shift; for l in "$@"; do printf '    %s\n' "$l" >&2; done; exit 1; }

# ---- 1. ssh 选项：新旧 OpenSSH 的那个选项换过名字 --------------------------
# 用 `ssh -G`（只解析配置、不连接）来判断本机认哪一个，避免硬编码。
if ssh -G -o PubkeyAcceptedAlgorithms=+ssh-rsa localhost >/dev/null 2>&1; then
	PUBKEY_OPT="-o PubkeyAcceptedAlgorithms=+ssh-rsa"
else
	PUBKEY_OPT="-o PubkeyAcceptedKeyTypes=+ssh-rsa"
fi

# shellcheck disable=SC2086   # 这些选项必须按词拆开传给 ssh
SSH_BASE="-o HostKeyAlgorithms=+ssh-rsa $PUBKEY_OPT -o ConnectTimeout=8 -o StrictHostKeyChecking=no"
[ -f "$SSH_KEY" ] && SSH_BASE="$SSH_BASE -i $SSH_KEY"

# shellcheck disable=SC2086
brd() { ssh $SSH_BASE "root@$BOARD" "$@"; }

step "[1/4] 板子可达性　$BOARD"
if ! ping -c 1 -t 3 "$BOARD" >/dev/null 2>&1 && ! ping -c 1 -W 3000 "$BOARD" >/dev/null 2>&1; then
	die "ping 不通 $BOARD" \
	    "· 板子可能还没起来（上电到就绪约 35 秒）" \
	    "· macOS 上 ARP 缓存常常是陈的： sudo arp -d $BOARD"
fi
ok "ping 通"

if ! STATUS=$(brd 'cat /media/sd-mmcblk1p2/hsm/HSM_STATUS' 2>/dev/null); then
	die "SSH 连不上（ping 是通的，所以不是网络问题）" \
	    "· 若报 Permission denied (publickey,password)：本机 OpenSSH 拒绝发 RSA(SHA-1) 签名。" \
	    "  本脚本已自动加了 $PUBKEY_OPT，仍失败就是这把钥匙没被板子接受：" \
	    "      SSH_KEY=~/.ssh/别的私钥 $0" \
	    "· 板子的公钥表在 /home/root/.ssh/authorized_keys"
fi
ok "SSH 通"

printf '%s\n' "$STATUS" | sed 's/^/    /'
if ! printf '%s\n' "$STATUS" | grep -q '^READY=yes'; then
	die "板子还没就绪（READY≠yes）" \
	    "· 刚上电的话等满 35 秒再跑一次" \
	    "· DAEMON=nosock 通常是缺 /media/sd-mmcblk1p2/hsm/hsm_token（daemon 是 fail-closed 的）"
fi
ok "READY=yes"

[ "$MODE" = status ] && exit 0

# ---- 2. 客户端：仓库里没有 CMake 目标，两个文件直接编 ----------------------
step "[2/4] 客户端 sdf_demo"
SRC1=$ROOT/service/sdf_demo.c
SRC2=$ROOT/service/libsdfe.c
[ -f "$SRC1" ] || die "找不到 $SRC1（这个脚本要在仓库里跑）"

if [ ! -x "$CLIENT" ] || [ "$SRC1" -nt "$CLIENT" ] || [ "$SRC2" -nt "$CLIENT" ]; then
	command -v cc >/dev/null 2>&1 || \
		die "没有 cc" "· macOS： xcode-select --install"
	mkdir -p "$(dirname "$CLIENT")"
	# 只链接 libsdfe —— 纯 socket，不依赖 OpenSSL / liboqs。
	# 这不是图省事：sdf_demo 自己算不出任何东西，它打印的每个正确值都出自 FPGA。
	cc -O2 -Wall -Wextra -I"$ROOT/service" -o "$CLIENT" "$SRC1" "$SRC2" \
		|| die "编译失败"
	ok "已编译　$CLIENT"
else
	ok "已是最新　$CLIENT"
fi

# ---- 3. 一次性口令 ---------------------------------------------------------
step "[3/4] 取一次性口令"
TOK=$(brd 'cat /media/sd-mmcblk1p2/hsm/hsm_token' 2>/dev/null || true)
[ -n "$TOK" ] || die "口令是空的" \
	"· 板上 /media/sd-mmcblk1p2/hsm/hsm_token 不存在或读不到" \
	"· 没有这个文件 daemon 就不监听（fail-closed），TCP $PORT 也不会开"
ok "拿到口令（${#TOK} 字符）"

# ---- 4. 跑 -----------------------------------------------------------------
step "[4/4] 远程调用　$BOARD:$PORT"
say ""
if [ "$MODE" = smoke ]; then
	OUT=$("$CLIENT" "$BOARD" "$TOK" "$PORT" 2>&1) || {
		printf '%s\n' "$OUT" >&2
		die "演示失败（退出码非 0）"
	}
	printf '%s\n' "$OUT" | grep -E '\[设备\]|共享密钥一致|GB/T|ML-DSA|旧句柄|全部完成' || true
	say ""
	printf '%s\n' "$OUT" | grep -q '全部完成' \
		|| die "没跑到最后一节" "· 完整输出： $CLIENT $BOARD <口令>"
	ok "冒烟通过"
else
	"$CLIENT" "$BOARD" "$TOK" "$PORT" || die "演示失败（退出码非 0）"
fi

say ""
say "───────────────────────────────────────────────"
say "演示时值得指出的两点："
say "  · 三个 VERSION 都是 0x00010000 —— 密码核真的在 PL 里活着；"
say "    读到 0 就是被防火墙拒了，或者位流没载。"
say "  · 最后一节是**反证**：断开重连后旧句柄必须失效，"
say "    证明句柄是硬件侧的，不是应用自己编的。"
