#!/bin/sh
# demo_remote.sh —— 在本机（Mac / 任意主机）调用密码机。**主路径不用 SSH。**
#
#   ./tools/demo_remote.sh <IP> --provision   # 只需做一次：生成 PKI、装板子、留凭据
#   ./tools/demo_remote.sh <IP>               # 之后就是纯本地：编译 + mTLS 调用
#   ./tools/demo_remote.sh <IP> --smoke       # 最小冒烟，只打关键结论
#   ./tools/demo_remote.sh <IP> --status      # 只连一下看设备在不在
#
# 地址与模式不分先后。地址取值顺序：命令行参数 → $BOARD 环境变量 →
# demo/remote/board.conf（`demo/remote/run.sh --save` 写的那份）。三样都没有
# 就打印用法退出 —— **不猜默认值**，猜错的表现是"连不上"，比"你没给地址"难查。
#
# 其余可用环境变量覆盖：
#   PORT=9797                   daemon 的 TCP 口
#   PQCHSM_PKI=<目录>           本机凭据目录（默认 ~/.config/pqchsm/pki）
#   DEVICE_CN=axu3egb-hsm-01    期望的设备证书 CN（连上后逐字比对）
#   SSH_KEY=~/.ssh/id_rsa       只有 --provision 会用到
#
# ============================================================================
# 【调用链 —— 密码运算一步都不经过 SSH】
# ============================================================================
#   本机 sdf_demo（原生二进制）
#     └─ libsdfe ──── mTLS / TCP 9797 ───▶ 板上 pqchsm_fpgad
#                                            └─ /dev/secmmio
#                                                 └─ EL3 SiP（安全世界）
#                                                      └─ FPGA 里的密码核
#
# `sdf_demo` **不链接任何密码算法库**：它链的 OpenSSL 只做 TLS 传输，
# 里面没有 ML-KEM / ML-DSA / SM4 / SM3 的任何实现。所以它打印出的每一个
# 对得上标准向量的数值都只能来自 FPGA。这是刻意的设计，也是演示时最该指出的一点。
#
# ============================================================================
# 【远程口现在是 mTLS，不再是明文口令】
# ============================================================================
# 2026-08-18 之前这条链路是**明文 TCP + 一条静态预共享口令**：同网段抓一次包
# 就永久接管，整条会话既不保密也不防篡改，认证帧还能重放。那条路已经删掉。
#
# 现在两侧都要出示证书（同一个设备 CA 签发）：
#   板上  /media/sd-mmcblk1p2/hsm/pki/{hsm_ca.crt,hsm_device.crt,hsm_device.key}
#   本机  ~/.config/pqchsm/pki/{hsm_ca.crt,client.crt,client.key}
#
# SSH 只在 `--provision` 里出现一次，用途是把设备凭据装到板上。装完之后
# **调用不需要板子的 shell、也不需要任何 SSH 凭据**。
#
# ⚠️ 真实部署里客户端证书应当带外分发（或由操作端自己生成 CSR 送签），
#    `--provision` 只是给手上就有板子 shell 的人省事。CA 私钥**只留在本机**，
#    一步都不上板。
set -eu

PORT=${PORT:-9797}
PKI_DIR=${PQCHSM_PKI:-$HOME/.config/pqchsm/pki}
DEVICE_CN=${DEVICE_CN:-axu3egb-hsm-01}
SSH_KEY=${SSH_KEY:-$HOME/.ssh/id_rsa}
BOARD_PKI=/media/sd-mmcblk1p2/hsm/pki
CONF=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/demo/remote/board.conf

MODE=full
ARG_BOARD=""
for a in "$@"; do
	case "$a" in
	--smoke)     MODE=smoke ;;
	--status)    MODE=status ;;
	--provision) MODE=provision ;;
	-h|--help)   sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) echo "未知参数：$a（用 --help 看用法）" >&2; exit 2 ;;
	*)
		[ -z "$ARG_BOARD" ] || { echo "地址给了不止一个：$ARG_BOARD 与 $a" >&2; exit 2; }
		ARG_BOARD=$a ;;
	esac
done

# 只在真的有人在敲键盘时问（-t 0）；管道/CI/cron 里回到"打印用法并退出"，
# 否则脚本会挂在那儿等一个永远不来的回车。
# ⚠️ 这里只问地址，**不把模式做成菜单** —— --provision 会重装板上的设备凭据，
#    那是要人明确打出来的动作，不是从列表里随手点中的一项。
ask_board() {
	[ -t 0 ] || {
		echo "没给板子地址。用法： $0 <板子IP> [--provision|--smoke|--status]" >&2
		echo "（也可以设 \$BOARD，或用 demo/remote/run.sh <IP> --save 记一次）" >&2
		exit 2
	}
	printf '\n\033[1m这台密码机在哪？\033[0m\n'
	printf '  板子的 IP 或主机名，例如 \033[2m192.168.1.50\033[0m（Ctrl-C 退出）\n\n'
	while :; do
		printf '  地址： '
		IFS= read -r ans || { printf '\n'; exit 2; }
		ans=$(printf '%s' "$ans" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
		case "$ans" in
		"") printf '  \033[33m·\033[0m 空的。直接给个 IP 就行\n' ;;
		*[!A-Za-z0-9.:_-]*)
		    printf '  \033[33m·\033[0m "%s" 不像个地址 —— 只要主机名或 IP，不带 http:// 和端口\n' "$ans" ;;
		*)  BOARD=$ans; return 0 ;;
		esac
	done
}

if   [ -n "$ARG_BOARD" ];  then BOARD=$ARG_BOARD
elif [ -n "${BOARD:-}" ];  then :
elif [ -f "$CONF" ];       then BOARD=$(tr -d ' \t\r\n' < "$CONF")
                                [ -n "$BOARD" ] || ask_board
else                            ask_board
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CLIENT=$ROOT/build-demo/sdf_demo
CA_SRC=$ROOT/pki                      # CA 与签发产物留在仓库外（.gitignore）

ok()   { printf '  \033[32m✓\033[0m %s\n' "$*"; }
step() { printf '\n\033[1m%s\033[0m\n' "$*"; }
die()  { printf '  \033[31m✗\033[0m %s\n' "$1" >&2; shift
         for l in "$@"; do printf '    %s\n' "$l" >&2; done; exit 1; }

# ---- --provision：唯一会用 SSH 的动作，一次性 ------------------------------
if [ "$MODE" = provision ]; then
	step "① 生成设备 PKI（CA 私钥只留在本机）"
	"$ROOT/tools/mkpki.sh" "$CA_SRC" "$DEVICE_CN" "$(whoami)@$(hostname -s)" \
		2>/dev/null || die "生成 PKI 失败" "· 需要 openssl"
	ok "PKI 在 $CA_SRC"

	step "② 把设备凭据装到板上（这是本脚本唯一用 SSH 的地方）"
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

	# 先建目录再逐个灌文件。**私钥用 stdin 送**，不落在命令行上
	# （命令行会进板子的 ps 输出与 shell 历史）。
	# shellcheck disable=SC2086
	ssh $SO "root@$BOARD" "mkdir -p $BOARD_PKI && chmod 700 $BOARD_PKI" \
		|| die "连不上板子 $BOARD" \
		   "· 板子可能还没起来（上电到就绪约 35 秒）" \
		   "· macOS 的 ARP 缓存常是陈的： sudo arp -d $BOARD"
	for f in hsm_ca.crt hsm_device.crt hsm_device.key; do
		# shellcheck disable=SC2086
		ssh $SO "root@$BOARD" "cat > $BOARD_PKI/$f && chmod 600 $BOARD_PKI/$f" \
			< "$CA_SRC/$f" || die "装 $f 失败"
	done
	ok "板上 $BOARD_PKI 已就位（0600）"

	step "③ 本机留一份客户端凭据"
	mkdir -p "$PKI_DIR"
	chmod 700 "$PKI_DIR"
	umask 077
	cp "$CA_SRC/hsm_ca.crt" "$CA_SRC/client.crt" "$CA_SRC/client.key" "$PKI_DIR/"
	chmod 600 "$PKI_DIR/client.key"
	ok "凭据在 $PKI_DIR"

	step "④ 对时（证书的有效期要求两边时钟是一致的）"
	# ⚠️ 这一步不是"顺手做的"，它是**必需**的，而且踩过：
	#    板子的时钟比本机慢了 5 天，于是刚签出来的客户端证书在板子看来
	#    "尚未生效"，握手被拒。更坑的是 TLS 1.3 的拒绝是**握手后**才送到的，
	#    客户端那侧 SSL_connect 照样成功，症状变成"设备信息是一串乱码"。
	#    （客户端那侧现在会在开设备时多走一个来回把这类拒绝逼出来，
	#      但根因还是得在这里解决。）
	#
	# 同时写进 RTC：这块板有 /dev/rtc，写了之后断电重启也还在。
	# shellcheck disable=SC2086
	ssh $SO "root@$BOARD" \
	    "date -u -s '$(date -u '+%Y-%m-%d %H:%M:%S')' >/dev/null && \
	     busybox hwclock -w -u 2>/dev/null; date -u" >/dev/null 2>&1 \
		|| die "对时失败" "· 板子上没有 date/hwclock？先手动看一眼 ssh root@$BOARD date"
	ok "板子时钟已与本机对齐（并写入 RTC）"

	step "⑤ 让 daemon 认新凭据（重启它，不重启板子）"
	# daemon 只在启动时读凭据。**不重启板子** —— 重启板子会重刷 PL，
	# 代价大得多，而且这一步失败时板子还在、还能连上去查。
	# shellcheck disable=SC2086
	ssh $SO "root@$BOARD" \
	    'killall pqchsm_fpgad 2>/dev/null; sleep 1;
	     setsid /media/sd-mmcblk1p2/hsm/pqchsm_fpgad -lock \
	       >> /media/sd-mmcblk1p2/hsm/hsm-boot.log 2>&1 < /dev/null &
	     sleep 3;
	     if busybox netstat -ltn 2>/dev/null | grep -q ":9797 "; then
	         echo TCP_UP; else echo TCP_DOWN; fi' 2>/dev/null | grep -q TCP_UP \
		|| die "daemon 没监听上 9797" \
		   "· 凭据可能没读到，看板上 /media/sd-mmcblk1p2/hsm/hsm-boot.log" \
		   "· 三样（hsm_ca.crt / hsm_device.crt / hsm_device.key）缺一就不监听"
	ok "daemon 已带新凭据重启，9797 在听"

	printf '\n以后直接跑 %s —— 不再需要 SSH。\n' "$0"
	exit 0
fi

# ---- 1. 凭据：本机三件套，不用 SSH -----------------------------------------
for f in hsm_ca.crt client.crt client.key; do
	[ -f "$PKI_DIR/$f" ] || die "本机缺 $PKI_DIR/$f" \
		"远程口是 mTLS，要一次性装凭据（唯一用 SSH 的动作）：" \
		"    $0 ${BOARD} --provision" \
		"（2026-08-18 之前那条 --fetch-token 的明文口令路已经删掉了）"
done

# ---- 2. 客户端：仓库里没有 CMake 目标，两个文件直接编 ----------------------
SRC1=$ROOT/service/sdf_demo.c
SRC2=$ROOT/service/libsdfe.c
SRC3=$ROOT/service/pqcs_tls.c
[ -f "$SRC1" ] || die "找不到 ${SRC1}（这个脚本要在仓库里跑）"

SSL_CFLAGS=""; SSL_LIBS="-lssl -lcrypto"
if [ "$(uname -s)" = "Darwin" ]; then
	# macOS 自带的是 LibreSSL，缺 TLS 1.3 的那套 API —— 用 Homebrew 的 openssl@3。
	# 找不到就当场说清楚，不要退回系统那份（那会在链接期报一堆看不懂的符号错）。
	P=$(brew --prefix openssl@3 2>/dev/null || true)
	[ -n "$P" ] || die "macOS 上需要 openssl@3" "    brew install openssl@3"
	SSL_CFLAGS="-I$P/include"; SSL_LIBS="-L$P/lib -lssl -lcrypto"
fi

NEED=0
[ -x "$CLIENT" ] || NEED=1
for s in "$SRC1" "$SRC2" "$SRC3"; do
	[ "$s" -nt "$CLIENT" ] && NEED=1
done
if [ "$NEED" = 1 ]; then
	command -v cc >/dev/null 2>&1 || die "没有 cc" "· macOS： xcode-select --install"
	mkdir -p "$(dirname "$CLIENT")"
	# shellcheck disable=SC2086
	cc -O2 -Wall -Wextra $SSL_CFLAGS -I"$ROOT/service" -o "$CLIENT" \
	   "$SRC1" "$SRC2" "$SRC3" $SSL_LIBS || die "编译失败"
	BUILT="刚编译"
else
	BUILT="已是最新"
fi

# ---- 3. 跑（纯 mTLS/TCP，无 SSH）-------------------------------------------
step "本机调用密码机　$BOARD:$PORT"
ok "客户端　${BUILT}（${CLIENT}）"
ok "凭据来自　${PKI_DIR}（mTLS，期望设备 CN=${DEVICE_CN}）"
printf '\n'

run() { "$CLIENT" "$BOARD" "$PKI_DIR" "$PORT" "$DEVICE_CN" 2>&1; }

# sdf_demo 自身对"连不上/凭据不对"都会打明确的话，所以不另做探测；
# 只在它没走到最后一节时，把可执行的下一步补上。
diagnose() {
	printf '%s\n' "$1" | grep -q 'TLS 握手失败\|设备证书\|设备身份不符' && \
		die "mTLS 没通过" \
		"· 板子换过凭据就要重装： $0 ${BOARD} --provision" \
		"· 设备 CN 不符时可以先用 DEVICE_CN=<板上证书的CN> $0 看一眼" \
		"· 板上 pki/ 三样缺一，daemon 就不监听 9797"
	printf '%s\n' "$1" | grep -q '打开设备失败' && die "连不上 $BOARD:$PORT" \
		"· 板子可能还没起来（上电到就绪约 35 秒）" \
		"· macOS 的 ARP 缓存常是陈的： sudo arp -d $BOARD" \
		"· 端口不对就设 PORT=…"
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
	printf '演示时值得指出的三点：\n'
	printf '  · 三个 VERSION 都是 0x00010000 —— 密码核真的在 PL 里活着；\n'
	printf '    读到 0 就是被防火墙拒了，或者位流没载。\n'
	printf '  · 这条链路是 **mTLS**：两侧都出示了证书。抓包只能看到密文，\n'
	printf '    而且没有客户端私钥就连不上 —— 换掉的正是原来那条明文口令。\n'
	printf '  · 最后一节是**反证**：断开重连后旧句柄必须失效，\n'
	printf '    证明句柄是硬件侧的，不是应用自己编的。\n'
	;;
esac
