#!/usr/bin/env bash
# 一键远程演示 —— 在任何一台够得着板子的机器上跑，不需要配置、不需要 SSH。
#
#     ./demo/remote/run.sh              完整九节演示
#     ./demo/remote/run.sh --smoke      只打关键结论
#     ./demo/remote/run.sh --status     只连一下看设备在不在
#
# 板子地址、端口、设备 CN 都可以用环境变量覆盖：
#     BOARD=192.168.50.175 PORT=9797 DEVICE_CN=axu3egb-hsm-01 ./demo/remote/run.sh
#
# ============================================================================
# ⚠️ 这里附带的客户端凭据是**演示专用、公开的**
# ============================================================================
# creds/ 下那三个文件（含私钥）就在这个公开仓库里，任何人都拿得到。这是有意的：
# 它把"跑一次演示"的门槛降到零。**代价必须说清楚：**
#
#   · 凭这份凭据，任何**够得着板子 9797 口**的人都能驱动这台密码机。
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

BOARD=${BOARD:-192.168.50.175}
PORT=${PORT:-9797}
DEVICE_CN=${DEVICE_CN:-axu3egb-hsm-01}
CREDS=${CREDS:-${HERE}/creds}
CLIENT=${ROOT}/build-demo/sdf_demo

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
ok()   { printf '  \033[32m✓\033[0m %s\n' "$1"; }
die()  { printf '  \033[31m✗\033[0m %s\n' "$1" >&2; shift
         for l in "$@"; do printf '    %s\n' "$l" >&2; done; exit 1; }

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

MODE=${1:---full}
case "${MODE}" in
--status) printf '%s\n' "${OUT}" | grep -E '^\[连接\]|^\[设备\]' || true ;;
--smoke)  printf '%s\n' "${OUT}" | grep -E '^\[设备\]|^  ✅|^===' || true ;;
--full)   printf '%s\n' "${OUT}" ;;
*)        die "不认识的参数：${MODE}" "用法： $0 [--full | --smoke | --status]" ;;
esac

# 判据是最后一节真的跑到了，不是退出码 —— sdf_demo 对"连不上/凭据不对"
# 自己会打明白话，所以这里只在它没走完时补上可执行的下一步。
if ! printf '%s\n' "${OUT}" | grep -q '全部完成'; then
    printf '\n'
    printf '%s\n' "${OUT}" | grep -q 'TLS 握手失败\|设备证书\|设备身份不符' && \
        die "mTLS 没通过" \
        "· 板子换过设备凭据了？让持有 CA 的人重跑 tools/demo_remote.sh --provision" \
        "· 设备 CN 不符： DEVICE_CN=<板上证书的CN> $0" \
        "· 两边时钟差太多也会这样（证书 notBefore 还没到）"
    printf '%s\n' "${OUT}" | grep -q '打开设备失败\|连接' && die "连不上 ${BOARD}:${PORT}" \
        "· 板子上电到就绪约 35 秒" \
        "· 不在同一网段？ BOARD=<板子IP> $0" \
        "· macOS 的 ARP 缓存常是陈的： sudo arp -d ${BOARD}"
    die "演示没跑到最后一节"
fi
printf '\n'
if [ "${MODE}" = --status ]; then
    ok "设备在线（三个 VERSION 都应是 0x00010000）"
else
    ok "演示通过 —— 上面每一个正确结果都只可能来自 FPGA"
fi
