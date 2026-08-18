#!/usr/bin/env bash
# 一键远程演示 —— 在任何一台够得着板子的机器上跑，不需要 SSH、不需要装凭据。
#
#     ./demo/remote/run.sh <板子IP|主机名> [--full|--smoke|--status]
#
#     ./demo/remote/run.sh 192.168.1.50            完整九节演示
#     ./demo/remote/run.sh 192.168.1.50 --smoke    只打关键结论
#     ./demo/remote/run.sh 192.168.1.50 --status   只连一下看设备在不在
#
# 地址不写死在脚本里 —— 这个仓库是公开的，别人的板子不在我们的网段上。
# 每次都打一遍嫌烦就存一次，之后省略即可：
#
#     ./demo/remote/run.sh 192.168.1.50 --save     写进 board.conf（已 gitignore）
#     ./demo/remote/run.sh --smoke                 之后不用再打地址
#
# 取地址的顺序：命令行参数 → $BOARD 环境变量 → demo/remote/board.conf。
# 三样都没有就打印用法退出 —— **不猜一个默认值**，猜错的表现是"连不上"，
# 比"你没给地址"难查得多。
#
# 端口、设备 CN、凭据目录同样可以覆盖：
#     PORT=9797 DEVICE_CN=axu3egb-hsm-01 CREDS=~/.config/pqchsm/pki \
#         ./demo/remote/run.sh 192.168.1.50
#
# ============================================================================
# ⚠️ 这里附带的客户端凭据是**演示专用、公开的**
# ============================================================================
# creds/ 下那三个文件（含私钥）就在这个公开仓库里，任何人都拿得到。这是有意的：
# 它把"跑一次演示"的门槛降到零。**代价必须说清楚：**
#
#   · 凭这份凭据，任何**够得着板子那个口**的人都能驱动这台密码机。
#     它成立的前提只有一条 —— 板子在内网、外面路由不到。
#   · 所以它**不是**远程口的安全论证。远程口的 mTLS 买到的是"持有私钥"而不是
#     "知道一个字符串"；把私钥公开发布，等于在这一条链路上自愿放弃那个区别。
#     真实部署要按 docs/SECURITY.md「远程口」一节做：自己生成 CA
#     （tools/mkpki.sh），凭据带外分发，必要时用板上的 hsm_acl 按 CN 限制。
#   · CA 私钥（pki/ca.key）与设备私钥**不在**仓库里，也永远不该进来 —— 有它们
#     就能签发新证书、冒充这台设备，那是另一个量级的事。
#
# 换句话说：**这份凭据是给"内网演示"用的，不要拿去部署。**
# ============================================================================
set -euo pipefail

HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "${HERE}/../.." && pwd)"
CONF="${HERE}/board.conf"

PORT=${PORT:-9797}
DEVICE_CN=${DEVICE_CN:-axu3egb-hsm-01}
CREDS=${CREDS:-${HERE}/creds}
CLIENT=${ROOT}/build-demo/sdf_demo

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
ok()   { printf '  \033[32m✓\033[0m %s\n' "$1"; }
die()  { printf '  \033[31m✗\033[0m %s\n' "$1" >&2; shift
         for l in "$@"; do printf '    %s\n' "$l" >&2; done; exit 1; }

usage() {
    cat >&2 <<USAGE
用法： $0 [板子IP|主机名] [--full|--smoke|--status] [--save]

    $0                          什么都不给 —— 会问你要地址（交互式）
    $0 192.168.1.50             完整九节演示
    $0 192.168.1.50 --smoke     只打关键结论
    $0 192.168.1.50 --status    只连一下看设备在不在
    $0 192.168.1.50 --save      记住这个地址，以后可以省略

板子地址不写死在脚本里。取值顺序：命令行参数 → \$BOARD → ${CONF} → 交互询问。
端口/设备 CN/凭据目录：PORT= DEVICE_CN= CREDS= 三个环境变量。
USAGE
    exit 2
}

# ---- 0. 参数：地址与模式不分先后，哪个在前都认 -----------------------------
ARG_BOARD=""; MODE=""; SAVE=0; ASKED=0
for a in "$@"; do
    case "${a}" in
    --full|--smoke|--status)
        [ -z "${MODE}" ] || die "模式给了不止一个：${MODE} 与 ${a}"
        MODE=${a} ;;
    --save)  SAVE=1 ;;
    -h|--help) usage ;;
    -*)      die "不认识的参数：${a}" "用法： $0 [板子IP] [--full|--smoke|--status] [--save]" ;;
    *)
        [ -z "${ARG_BOARD}" ] || die "地址给了不止一个：${ARG_BOARD} 与 ${a}"
        ARG_BOARD=${a} ;;
    esac
done

# 地址长得像不像个地址。宽松是有意的（主机名、.local、IPv6 都要能过），
# 只把明显不是地址的挡掉 —— 空格、引号、斜杠这类多半是复制粘贴粘歪了。
looks_like_address() {
    case "$1" in
    ""|*[!A-Za-z0-9.:_-]*) return 1 ;;
    *) return 0 ;;
    esac
}

# 交互问一次。只在**真的有人在敲键盘**时问（-t 0）；管道里、CI 里、
# cron 里一律回到"打印用法并退出"，否则脚本会挂在那儿等一个永远不来的回车。
ask_board() {
    [ -t 0 ] || { printf '  \033[31m✗\033[0m 没给板子地址\n\n' >&2; usage; }
    ASKED=1
    printf '\n'; bold "这台密码机在哪？"
    printf '  板子的 IP 或主机名，例如 \033[2m192.168.1.50\033[0m 或 \033[2mhsm.local\033[0m\n'
    printf '  （板子的 eth1 是静态地址，上电约 35 秒后就绪；Ctrl-C 退出）\n\n'
    while :; do
        printf '  地址： '
        IFS= read -r ans || { printf '\n'; die "没读到输入"; }
        # 只去掉首尾空白（含粘贴带来的 \r），中间不动 —— 报错时要把人**真正
        # 输入的**东西原样回显出去，改过的字符串会让人以为自己没打错。
        ans=${ans#"${ans%%[![:space:]]*}"}
        ans=${ans%"${ans##*[![:space:]]}"}
        if looks_like_address "${ans}"; then BOARD=${ans}; return 0; fi
        if [ -z "${ans}" ]; then
            printf '  \033[33m·\033[0m 空的。直接给个 IP 就行，比如 192.168.1.50\n'
        else
            printf '  \033[33m·\033[0m "%s" 不像个地址 —— 只要主机名或 IP，不带 http://、端口和路径\n' "${ans}"
            printf '    （端口用 PORT=… 覆盖，默认 %s）\n' "${PORT}"
        fi
    done
}

# 没指定模式又是交互式，就把三种形态摆出来让人选 —— 不必先去读用法。
ask_mode() {
    [ -t 0 ] || { MODE=--full; return 0; }
    printf '\n'; bold "跑哪一个？"
    printf '    1) 完整九节演示　　　　\033[2m默认\033[0m\n'
    printf '    2) 只打关键结论　　　　\033[2m--smoke\033[0m\n'
    printf '    3) 只连一下看在不在　　\033[2m--status\033[0m\n\n'
    while :; do
        printf '  选 [1-3，回车= 1]： '
        IFS= read -r ans || { printf '\n'; MODE=--full; return 0; }
        case "$(printf '%s' "${ans}" | tr -d ' \t\r')" in
        ""|1) MODE=--full;   return 0 ;;
        2)    MODE=--smoke;  return 0 ;;
        3)    MODE=--status; return 0 ;;
        *)    printf '  \033[33m·\033[0m 只认 1 / 2 / 3\n' ;;
        esac
    done
}

# 命令行 → 环境变量 → 存过的那份 → 问人。一层都不猜。
if   [ -n "${ARG_BOARD}" ]; then BOARD=${ARG_BOARD}
elif [ -n "${BOARD:-}" ];   then :
elif [ -f "${CONF}" ];      then BOARD=$(tr -d ' \t\r\n' < "${CONF}")
                                 [ -n "${BOARD}" ] || ask_board
else                             ask_board
fi

[ -n "${MODE}" ] || { [ "${ASKED}" = 1 ] && ask_mode; }
MODE=${MODE:---full}

if [ "${SAVE}" = 1 ]; then
    printf '%s\n' "${BOARD}" > "${CONF}"
    ok "已记住 ${BOARD}（${CONF}，不进仓库）"
fi

# ---- 1. 凭据 ---------------------------------------------------------------
for f in hsm_ca.crt client.crt client.key; do
    [ -f "${CREDS}/${f}" ] || die "缺 ${CREDS}/${f}" \
        "· 这三个文件本该随仓库一起来，checkout 不完整？" \
        "· 用自己那套凭据： CREDS=~/.config/pqchsm/pki $0"
done

# ---- 2. 编译客户端（只有三个文件，不用 CMake）------------------------------
SRC="${ROOT}/service/sdf_demo.c ${ROOT}/service/libsdfe.c ${ROOT}/service/pqcs_tls.c"
for s in ${SRC}; do
    [ -f "${s}" ] || die "找不到 ${s}" "· 这个脚本要在仓库里跑"
done

SSL_CFLAGS=""; SSL_LIBS="-lssl -lcrypto"
if [ "$(uname -s)" = "Darwin" ]; then
    # macOS 自带的是 LibreSSL，缺 TLS 1.3 的那套 API —— 必须用 Homebrew 的
    # openssl@3。找不到就当场说清楚，不要退回系统那份（那会在链接期报一堆
    # 看不懂的符号错，比现在这句话难查得多）。
    P=$(brew --prefix openssl@3 2>/dev/null || true)
    [ -n "${P}" ] || die "macOS 上需要 openssl@3" "    brew install openssl@3"
    SSL_CFLAGS="-I${P}/include"; SSL_LIBS="-L${P}/lib -lssl -lcrypto"
fi

NEED=0
[ -x "${CLIENT}" ] || NEED=1
for s in ${SRC}; do [ "${s}" -nt "${CLIENT}" ] && NEED=1; done
if [ "${NEED}" = 1 ]; then
    command -v cc >/dev/null 2>&1 || die "没有 cc" "· macOS： xcode-select --install" \
                                            "· Debian/Ubuntu： apt install build-essential libssl-dev"
    mkdir -p "$(dirname "${CLIENT}")"
    # shellcheck disable=SC2086
    cc -O2 -Wall -Wextra ${SSL_CFLAGS} -I"${ROOT}/service" -o "${CLIENT}" \
       ${SRC} ${SSL_LIBS} || die "编译失败" \
       "· 需要 OpenSSL 3 的开发头文件（TLS 1.3 API）"
    BUILT="刚编译"
else
    BUILT="已是最新"
fi

# ---- 3. 跑（纯 mTLS/TCP，全程不用 SSH）-------------------------------------
printf '\n'; bold "远程调用密码机　${BOARD}:${PORT}"
ok "客户端　${BUILT}（${CLIENT}）"
ok "凭据　　${CREDS}（演示专用，公开；期望设备 CN=${DEVICE_CN}）"
printf '\n'

OUT=$("${CLIENT}" "${BOARD}" "${CREDS}" "${PORT}" "${DEVICE_CN}" 2>&1) || true

case "${MODE}" in
--status) printf '%s\n' "${OUT}" | grep -E '^\[连接\]|^\[设备\]' || true ;;
--smoke)  printf '%s\n' "${OUT}" | grep -E '^\[设备\]|^  ✅|^===' || true ;;
--full)   printf '%s\n' "${OUT}" ;;
esac

# 判据是最后一节真的跑到了，不是退出码 —— sdf_demo 对"连不上/凭据不对"
# 自己会打明白话，所以这里只在它没走完时补上可执行的下一步。
if ! printf '%s\n' "${OUT}" | grep -q '全部完成'; then
    printf '\n'
    printf '%s\n' "${OUT}" | grep -q 'TLS 握手失败\|设备证书\|设备身份不符' && \
        die "mTLS 没通过" \
        "· 板子换过设备凭据了？让持有 CA 的人重跑 tools/demo_remote.sh ${BOARD} --provision" \
        "· 设备 CN 不符： DEVICE_CN=<板上证书的CN> $0 ${BOARD}" \
        "· 两边时钟差太多也会这样（证书 notBefore 还没到）"
    printf '%s\n' "${OUT}" | grep -q '打开设备失败\|连接' && die "连不上 ${BOARD}:${PORT}" \
        "· 板子上电到就绪约 35 秒" \
        "· 地址打错了？ $0 <板子IP> --status" \
        "· macOS 的 ARP 缓存常是陈的： sudo arp -d ${BOARD}"
    die "演示没跑到最后一节"
fi
printf '\n'
if [ "${MODE}" = --status ]; then
    ok "设备在线（三个 VERSION 都应是 0x00010000）"
else
    ok "演示通过 —— 上面每一个正确结果都只可能来自 FPGA"
fi

# 问过地址、又还没存过，才提议记住它 —— 而且只在**跑通之后**问：
# 存一个连不上的地址，下次就是拿着一个错答案再撞一次墙。
if [ "${ASKED}" = 1 ] && [ ! -f "${CONF}" ] && [ -t 0 ]; then
    printf '\n  以后记住 \033[1m%s\033[0m 吗？下次直接 %s --smoke 就行 [Y/n] ' "${BOARD}" "$0"
    IFS= read -r ans || ans=n
    case "$(printf '%s' "${ans}" | tr -d ' \t\r')" in
    ""|y|Y|yes|YES)
        printf '%s\n' "${BOARD}" > "${CONF}"
        ok "记住了（${CONF}，不进仓库）" ;;
    *)  printf '  没记。下次再问一遍，或者 %s %s --save\n' "$0" "${BOARD}" ;;
    esac
fi
