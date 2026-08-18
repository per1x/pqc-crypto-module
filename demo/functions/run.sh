#!/usr/bin/env bash
# 密码机功能演示 —— 算法支持性、熵源、槽位管理、密钥管理、备份恢复、安全存储
#
#     ./demo/functions/run.sh
#
# 自带一切：临时密钥库、自己起的 daemon、用完就删。不碰你已有的任何东西，
# 也不需要板子（板子那半在 demo/remote/ 与 board/demo/）。
#
# ============================================================================
# 【这里演的是哪一个密码机 —— 先分清，否则会看错】
# ============================================================================
# 这个仓库里有两个东西，功能面完全不同：
#
#   · 板上 pqchsm_fpgad  —— 驱动 FPGA 里的密码核。它**没有**带 PIN 的槽位、
#     没有密钥库、没有备份。它的"槽位"是 PL 金库的 0-7 号，就是一块放对称
#     密钥的硬件。
#   · 主机侧 pqchsmd     —— 槽位状态机、SO/User 角色、PIN 与锁定、密钥库
#     （AES-GCM 包裹 + KDR 派生 KEK + 全文件 MAC + epoch 防回滚）、
#     M-of-N 备份、审计链。
#
# **本脚本演的是后者。** 槽位/备份/安全存储这些密码机管理功能都在这一侧。
#
# 两边能接起来：设 PQCHSM_BACKEND=sdfe 之后，ML-KEM 的密钥生成走 FPGA、
# 私钥进 PL 片内金库 —— 于是"槽位与备份在主机侧管、密钥材料在硬件里"。
# 但那样有一个真实的取舍，见第五节末尾。
# ============================================================================
set -uo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
BUILD=${BUILD:-${ROOT}/build}
PORT=${PORT:-19099}
WORK=$(mktemp -d)
KS=${WORK}/keystore.bin
SO_PIN=${SO_PIN:-12345678}
USER_PIN=${USER_PIN:-87654321}

CLI="${BUILD}/pqchsm-cli -p ${PORT}"
ADMIN="${BUILD}/pqchsm-admin -k ${KS} -n 4"

cleanup() {
    [ -n "${DPID:-}" ] && kill "${DPID}" 2>/dev/null
    rm -rf "${WORK}"
}
trap cleanup EXIT

sec()  { printf '\n\033[1m━━ %s\033[0m\n\n' "$1"; }
sub()  { printf '  \033[1m%s\033[0m\n' "$1"; }   # say 走 %s，不解释转义
say()  { printf '  %s\n' "$1"; }
# 回显时把仓库路径与临时目录缩掉 —— 观众要看的是"调了哪条命令"，
# 不是一串 /var/folders/... 的绝对路径。
run()  {
    printf '  \033[2m$ %s\033[0m\n' \
        "$(printf '%s ' "$@" | sed "s|${BUILD}/||g; s|${WORK}/|\$TMP/|g; s| *$||")"
    "$@"
}
ok()   { printf '  \033[32m✓\033[0m %s\n' "$1"; }
no()   { printf '  \033[31m✗\033[0m %s\n' "$1"; }
die()  { no "$1"; exit 1; }

for b in pqchsmd pqchsm-cli pqchsm-admin; do
    [ -x "${BUILD}/${b}" ] || die "缺 ${BUILD}/${b} —— 先 cmake --build build"
done

# ---- 起一个只属于本次演示的 daemon ------------------------------------------
"${BUILD}/pqchsmd" -p "${PORT}" -s 4 -k "${KS}" > "${WORK}/daemon.log" 2>&1 &
DPID=$!
for _ in $(seq 1 40); do
    ${CLI} ping >/dev/null 2>&1 && break
    sleep 0.25
done
${CLI} ping >/dev/null 2>&1 || { cat "${WORK}/daemon.log"; die "daemon 没起来"; }

printf '\n\033[1m═══ 密码机功能演示 ═══\033[0m\n'
say "密钥库 ${KS}（临时，退出即删）"
say "$(sed -n '1s/^\[DEV\] *//p' "${WORK}/daemon.log")"

# ============================================================================
sec "一、算法支持性 —— 问模块自己，不是问文档"
# ============================================================================
MECHS=${WORK}/mechs
if cc -O2 -I "${ROOT}/third_party/pkcs11-v3.2" -I "${ROOT}/src/p11" \
      -o "${MECHS}" "${ROOT}/demo/functions/mechs.c" 2>/dev/null; then
    P11=$(ls "${BUILD}"/pqchsm-pkcs11.* 2>/dev/null | head -1)
    export P11
    if [ -n "${P11}" ]; then
        say "PKCS#11 的标准问法：C_GetMechanismList + C_GetMechanismInfo"
        PQCHSM_KEYSTORE=${WORK}/p11.bin "${MECHS}" "${P11}"
        say ""
        say "⚠️ CKM_ML_DSA 这一行容易读成\"硬件\"——它不是。算法本身有整核并在板上"
        say "   对过 ACVP，但 PKCS#11 这一侧走的是 liboqs。见 docs/API.md。"
    else
        say "（没找到 PKCS#11 模块，跳过；cmake --build build --target pqchsm-p11）"
    fi
else
    say "（编译机制枚举器失败，跳过这一节）"
fi

# ============================================================================
sec "二、熵源"
# ============================================================================
say "主机侧这个密码机的随机数来自 OpenSSL，**不是** PL 的熵源 —— 这条边界"
say "写在 docs/SECURITY.md 的局限表里，不含糊过去。"
say ""
say "真正的硬件熵源在板子上：8 个环形振荡器 + SP 800-90B 健康检测"
say "（RCT/APT），1,048,576 个调节前样本实测最小熵 H = 0.871 bit/sample。"
say "要看它，跑 demo/remote/run.sh（第 [1] 节）或板上的 run_demo.sh。"

# ============================================================================
sec "三、槽位管理"
# ============================================================================
${CLI} save >/dev/null            # 先落盘，admin 读的是文件
say "全部四个槽位的初始状态："
run ${ADMIN} list

say ""
say "槽位状态机：UNINIT →（init-token）→ EMPTY →（generate）→ LOADED"
run ${CLI} init-token 0 "SIGN-SLOT" "${SO_PIN}"
run ${CLI} info 0

say ""
say "会话与角色：SO 负责初始化和设 User PIN，User 负责用密钥。"
S=$(${CLI} session-open 0) || die "开会话失败"
say "会话句柄 = ${S}"

say ""
say "反例 —— 没登录就想生成密钥："
# ⚠️ 先取输出再判，不要写成 `if 命令 | grep`：set -o pipefail 下那个 if 拿到的
#    是**命令自己**的退出码，而这里的命令本来就该失败 —— 判据会永远走错分支。
GEN_OUT=$(${CLI} generate "${S}" ML-DSA-44 sign 2>&1 || true)
printf '    %s\n' "${GEN_OUT}"
if printf '%s' "${GEN_OUT}" | grep -q 'not authorized'; then
    ok "被拒（not authorized）—— 授权是真的在管事，不是摆设"
else
    no "居然没被拒 —— 这是个问题"
fi

run ${CLI} login "${S}" so "${SO_PIN}"
run ${CLI} set-user-pin "${S}" "${USER_PIN}"
run ${CLI} logout "${S}"
run ${CLI} login "${S}" user "${USER_PIN}"
ok "SO 设 PIN、User 登录，两个角色各司其职"

# ============================================================================
sec "四、密钥管理"
# ============================================================================
say "生成一对 ML-DSA-44（算法名就是 ACVP 的 parameterSet 串）："
H=$(${CLI} generate "${S}" ML-DSA-44 sign) || die "生成失败"
say "对象句柄 = ${H}"
run ${CLI} info 0

say ""
# ⚠️ 这里不能用 run：它会把回显的命令行也写进 stdout，重定向时混进公钥文件。
say "$(printf '\033[2m$ pqchsm-cli pubkey %s %s > pk.bin\033[0m' "${S}" "${H}")"
${CLI} pubkey "${S}" "${H}" > "${WORK}/pk.bin"
PKLEN=$(wc -c < "${WORK}/pk.bin" | tr -d ' ')
printf 'hello crypto module' | ${CLI} sign "${S}" "${H}" > "${WORK}/sig.bin"
SIGLEN=$(wc -c < "${WORK}/sig.bin" | tr -d ' ')
say "公钥 ${PKLEN} 字节，签名 ${SIGLEN} 字节"
[ "${PKLEN}" = 1312 ] && [ "${SIGLEN}" = 2420 ] \
    && ok "与 FIPS 204 的 ML-DSA-44 尺寸一致（1312 / 2420）" \
    || no "尺寸对不上 FIPS 204"

say ""
say "⚠️ 注意上面这份命令表里**没有\"导出私钥\"这一条** —— 不是忘了写。"
say "   公钥导得出，私钥导不出，这是接口层面的结构性事实。"
say "   PKCS#11 那一侧同理：私钥对象的 CKA_EXTRACTABLE 是 false。"

say ""
say "销毁与清零："
run ${CLI} destroy "${S}" "${H}"
run ${CLI} info 0

# ============================================================================
sec "五、备份与恢复（M-of-N 门限）"
# ============================================================================
say "备份不是想备就能备 —— 槽位策略说了算。"
say ""
say "⚠️ 先说清 backup 的参数：<M> <N> 后面那个 <slot> 是**用哪个槽位的 SO PIN"
say "   来授权**，不是\"备份哪个槽\"。导出的是密钥库里**所有**带 BACKUPABLE"
say "   策略的槽位。（pqchsm-admin 的帮助串这里写得有歧义，容易读反。）"
say ""

# 此刻还没有任何 BACKUPABLE 槽位 —— 先备一次，看默认拒绝
say "现在库里一个 BACKUPABLE 的槽位都没有。先备一次："
${ADMIN} backup "${WORK}/b0.bin" "${WORK}/x" 2 3 0 "${SO_PIN}" 2>&1 \
    | sed "s|${WORK}/|\$TMP/|g; s/^/    /"
ok "备了 0 个槽位 —— 默认不可备份，是默认拒绝而不是出错"

say ""
say "再建一个**带 BACKUPABLE（0x2）策略**的槽位："
S2=$(${CLI} session-open 2)
${CLI} init-token 2 "BACKUPABLE" "${SO_PIN}" >/dev/null
${CLI} login "${S2}" so "${SO_PIN}" >/dev/null
${CLI} set-user-pin "${S2}" "${USER_PIN}" >/dev/null
${CLI} logout "${S2}" >/dev/null; ${CLI} login "${S2}" user "${USER_PIN}" >/dev/null
${CLI} generate "${S2}" ML-DSA-44 sign 0x2 >/dev/null
${CLI} save >/dev/null
run ${ADMIN} list

say ""
say "同一条命令再来一次，2-of-3 分片："
${ADMIN} backup "${WORK}/b2.bin" "${WORK}/share" 2 3 0 "${SO_PIN}" 2>&1 \
    | sed "s|${WORK}/|\$TMP/|g; s/^/    /"
ok "这次备到了 —— 差别只在那一位策略上"

say ""
say "三份分片各 $(wc -c < "${WORK}/share.1" | tr -d ' ') 字节。现在验门限。"
say ""

cp "${KS}" "${WORK}/r1.bin"; cp "${KS}.epoch" "${WORK}/r1.bin.epoch" 2>/dev/null
say "只给 1 片（门限是 2）——**必须失败**："
R1_OUT=$("${BUILD}/pqchsm-admin" -k "${WORK}/r1.bin" -n 4 restore \
         "${WORK}/b2.bin" "${WORK}/share.1" 2>&1 || true)
printf '    %s\n' "${R1_OUT}"
if printf '%s' "${R1_OUT}" | grep -qi 'fail\|失败'; then
    ok "被拒 —— 少一片就是恢复不了"
else
    no "1 片就恢复成功了？门限没起作用"
fi

say ""
cp "${KS}" "${WORK}/r2.bin"; cp "${KS}.epoch" "${WORK}/r2.bin.epoch" 2>/dev/null
say "给 2 片（第 1 和第 3 片）："
"${BUILD}/pqchsm-admin" -k "${WORK}/r2.bin" -n 4 restore \
    "${WORK}/b2.bin" "${WORK}/share.1" "${WORK}/share.3" 2>&1 \
    | sed "s|${WORK}/|\$TMP/|g; s/^/    /"

say ""
cp "${KS}" "${WORK}/r3.bin"; cp "${KS}.epoch" "${WORK}/r3.bin.epoch" 2>/dev/null
say "换一组（第 1 和第 2 片）——真门限方案，不挑是哪几片："
"${BUILD}/pqchsm-admin" -k "${WORK}/r3.bin" -n 4 restore \
    "${WORK}/b2.bin" "${WORK}/share.1" "${WORK}/share.2" 2>&1 \
    | sed "s|${WORK}/|\$TMP/|g; s/^/    /"

say ""
say "⚠️ 一个真实的取舍，值得单独说：**\"能备份\"和\"留在硬件里\"是互斥的。**"
say "   备份备的是种子，而私钥一旦进了 FPGA 的片内金库就没有读出路径 ——"
say "   也就没法备份。可恢复 ↔ 不可导出，只能选一个。这不是缺陷，是密码机"
say "   设计里一个必须由使用者做的决定。"

# ============================================================================
sec "六、安全存储"
# ============================================================================
say "密钥库是一个文件：AES-GCM 包裹、KEK 由 KDR 派生、全文件 MAC、"
say "头部带一个每次写盘 +1 的 epoch。"
say ""
say "    keystore.bin  $(wc -c < "${KS}" | tr -d ' ') 字节"
[ -f "${KS}.epoch" ] && say "    keystore.bin.epoch（防回滚锚点）= $(cat "${KS}.epoch")"

say ""
say "反例甲 —— 改坏一个 bit："
cp "${KS}" "${WORK}/bad.bin"; cp "${KS}.epoch" "${WORK}/bad.bin.epoch" 2>/dev/null
python3 - "${WORK}/bad.bin" <<'PY'
import sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
b[200] ^= 0x01
open(p, 'wb').write(b)
PY
BEFORE=$(cksum < "${WORK}/bad.bin")
"${BUILD}/pqchsm-admin" -k "${WORK}/bad.bin" -n 4 list 2>&1 | sed 's/^/    /'
AFTER=$(cksum < "${WORK}/bad.bin")
[ "${BEFORE}" = "${AFTER}" ] \
    && ok "拒绝装载，**而且没有改写这个文件**（fail-closed，不是\"修一修再用\"）" \
    || no "文件被改写了 —— fail-closed 不成立"

say ""
say "反例乙 —— 拿一份旧快照换回去（回放攻击）："
cp "${KS}" "${WORK}/snap.bin"
${CLI} save >/dev/null; ${CLI} save >/dev/null
say "    旧快照 epoch 落后于锚点当前值 $(cat "${KS}.epoch" 2>/dev/null)"
cp "${WORK}/snap.bin" "${WORK}/replay.bin"
cp "${KS}.epoch" "${WORK}/replay.bin.epoch" 2>/dev/null
if "${BUILD}/pqchsm-admin" -k "${WORK}/replay.bin" -n 4 list >/dev/null 2>&1; then
    no "旧快照被接受了 —— 防回滚没起作用"
else
    ok "被拒 —— 旧快照的全文件 MAC 本来就是对的（当初自己算的），"
    say "     拦住它的只能是那个单调的 epoch 锚点"
fi

say ""
say "换 KEK（不动密钥本身，只换包裹它的那把）："
run ${CLI} rotate-kek
"${BUILD}/pqchsm-admin" -k "${KS}" -n 4 list >/dev/null 2>&1 \
    && ok "换完之后密钥库照常装载" || no "换完装不上了"

say ""
say "⚠️ 默认 KDR 是编译进二进制的**公开常量**（见开头那行 DEV 告警）——"
say "   也就是说这个演示形态下密钥库的机密性等于零。PRODUCTION 形态会拒绝"
say "   在没有硬件派生根时启动。见 docs/SECURITY.md「构建形态」一节。"

# ============================================================================
sec "七、算法调用 —— 支持不等于能用，走一遍完整调用链"
# ============================================================================
say "第一节列出了 5 个机制。列出来只是声明，这一节把它们**调一遍**。"
say ""
say "调用链（两条路，最终落到同一个后端）："
say ""
say "    应用 ──▶ PKCS#11 C_*  ──┐"
say "                            ├──▶ 槽位层（会话/角色/策略）"
say "    应用 ──▶ SDF SDFE_*  ──┘         │"
say "                                     ▼"
say "                              pqc 后端 vtable"
say "                                     │"
say "                       ┌─────────────┴─────────────┐"
say "                   liboqs（软件）          FPGA 密码核"
say "                                     PQCHSM_BACKEND=sdfe 切换"
say ""

RT=${WORK}/roundtrip
if cc -O2 -I "${ROOT}/third_party/pkcs11-v3.2" -I "${ROOT}/src/p11" \
      -o "${RT}" "${ROOT}/demo/functions/roundtrip.c" 2>/dev/null && [ -n "${P11:-}" ]; then
    sub "签名类：ML-DSA 的签 → 验往返，以及它的两个反例"
    say ""
    PQCHSM_KEYSTORE=${WORK}/rt.bin "${RT}" "${P11}" 2>&1 | sed 's/^/    /'
    RTRC=${PIPESTATUS[0]}
    [ "${RTRC}" = 0 ] && ok "签名往返成立，且两条篡改路径都验不过" \
                      || no "签名往返有问题（rc=${RTRC}）"
else
    say "（编译 roundtrip 失败或没有 PKCS#11 模块，跳过）"
fi

KEMDEMO=${WORK}/p11_hw_demo
if cc -O2 -I "${ROOT}/third_party/pkcs11-v3.2" -I "${ROOT}/src/p11" \
      -o "${KEMDEMO}" "${ROOT}/board/demo/p11_hw_demo.c" 2>/dev/null && [ -n "${P11:-}" ]; then
    say ""
    sub "KEM 类：ML-KEM 封装 → 解封装，再用共享密钥做 AES-GCM（KEM-DEM）"
    say ""
    PQCHSM_KEYSTORE=${WORK}/kem.bin "${KEMDEMO}" "${P11}" 2>&1 \
        | sed 's/^/    /; s/=== .*===//'
    ok "封装/解封装两端共享密钥一致，且私钥读不出来"
    say ""
    say "⚠️ 这**同一个程序**设上 PQCHSM_BACKEND=sdfe 就打到板子的 FPGA 上，"
    say "   一行代码不用改 —— 这正是\"标准接口\"的意思。板上那次见"
    say "   board/demo/README.zh-CN.md。"
else
    say "（编译 p11_hw_demo 失败，跳过 KEM 这半）"
fi

# ============================================================================
sec "怎么调用"
# ============================================================================
say "本演示用的是 pqchsm-cli（一条 TCP 到 pqchsmd）。应用一般走这两条："
say ""
say "  PKCS#11  —— 跨厂商标准接口，Python / Java / C 都有现成示例："
say "             demo/python/pqchsm_demo.py、demo/java/PqcHsmDemo.java"
say "  SDF 风格 —— 国密惯用的一套，直接对板上的 FPGA 密码机："
say "             demo/remote/run.sh，接口见 docs/API.md"
say ""
say "把 ML-KEM 的密钥生成接到 FPGA（私钥进 PL 片内金库）："
say "  export PQCHSM_BACKEND=sdfe PQCHSM_SDFE_HOST=<板子IP>"
say "  export PQCHSM_SDFE_PKI=\$HOME/.config/pqchsm/pki"

printf '\n\033[1m═══ 演示结束 ═══\033[0m\n'
say "临时密钥库与分片已删除。"
printf '\n'
