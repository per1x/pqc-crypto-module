#!/bin/sh
# 板上演示入口（部署到 /media/sd-mmcblk1p2/demo/run.sh）
#
#     ssh root@<板子IP>
#     sh /media/sd-mmcblk1p2/demo/run.sh            # 主线演示，约 1 分钟
#     sh /media/sd-mmcblk1p2/demo/run.sh --mgmt     # 另附：软件密码机的管理功能面
#
# 演示只讲一件事：**密钥在 TEE 里管，运算在 FPGA 里做，应用只调接口。**
#
#   〇 架构一页纸            谁持有什么、谁做什么
#   一 设备就绪              开机 35 秒自动就绪；各层身份
#   二 走一遍（demo_tour）   A 密钥管理在 TEE / B FPGA 做运算 / C 会话结束即擦
#   三 刚才那些调用走了哪条路 从 daemon 日志里摘出本次运行的每一笔
#   四 从硬件侧核对          会话结束金库为空；普通世界直接写 FPGA 装不进密钥
#   五 上层怎么调用          SDF C 接口 / PKCS#11 / 远程 mTLS
#
# 全程不改启动配置、不换位流。第四节往 FPGA 的装载口写 11 笔 —— 演示位流下
# 那是一道只认安全世界的门，写会被丢弃并计数（不会触发总线错误），这正是要看的。
set -u

HSM=/media/sd-mmcblk1p2/hsm
DEMO=$(cd "$(dirname "$0")" && pwd)
LOG=$HSM/hsm-boot.log          # daemon 的 stderr 追加在这里

B=$(printf '\033[1m'); N=$(printf '\033[0m'); G=$(printf '\033[32m'); R=$(printf '\033[31m')
part() { echo; echo "${B}════ $* ════${N}"; echo; }

echo
echo "${B}  AXU3EGB 后量子密码机 —— 演示${N}"
echo "  密钥在 TEE 里管 · 运算在 FPGA 里做 · 应用只调接口"

# ---------------------------------------------------------------- 〇 架构
part "〇 架构一页纸"
cat <<'EOF'
   ┌──────────────────────── 普通世界（Linux）────────────────────────┐
   │  应用 ──▶ libsdfe（SDF 接口）/ PKCS#11 ──▶ pqchsm_fpgad（daemon）  │
   │                                            · 串行化硬件访问          │
   │                                            · 句柄表里只有密文 blob   │
   └──────────────┬──────────────────────────────────┬────────────────┘
                  │ 密钥管理（TEE Client API）        │ 运算数据（寄存器读写）
   ┌──────────────▼──────── 安全世界 ──────────┐      │
   │  OP-TEE TA（S-EL0）                        │      │
   │   · 生成种子 / 对称密钥（TRNG）            │      │
   │   · 用 KEK 包成 blob 交出（KEK 不出 TA）   │      │
   │   · 需要时解开，经安全通路送进 FPGA        │      │
   │  PTA（S-EL1）──▶ BL31 / EL3（SiP 白名单）  │      │
   └──────────────┬─────────────────────────────┘      │
                  │ 只认安全事务的装载口               │ 经 EL3 白名单
   ┌──────────────▼──────────── FPGA（PL）─────────────▼──────────────┐
   │  ML-KEM · ML-DSA · AES / SM4 · SM3 · TRNG                         │
   │  · 私钥每次从种子现展开，算完由硬件无条件擦除                       │
   │  · 对称密钥在金库里没有任何读出路径，会话结束即擦                   │
   └───────────────────────────────────────────────────────────────────┘
EOF
echo
echo "  一句话：${B}明文密钥只存在于 TA 和 FPGA 里${N}；应用和 daemon 手里只有句柄和密文 blob。"

# ---------------------------------------------------------------- 一 就绪
part "一 设备就绪"
if [ -r "$HSM/HSM_STATUS" ]; then
    grep -E "^(PL|SECMMIO|TA|DAEMON|TCP|READY)=" "$HSM/HSM_STATUS" | sed 's/^/  /'
else
    echo "  读不到 HSM_STATUS —— 板子可能还没起完（上电约 35 秒）"
    exit 1
fi
OPTEE=$(dmesg 2>/dev/null | grep -o "optee: revision [^ ]*" | head -1)
echo
echo "  安全世界：${OPTEE:-OP-TEE（dmesg 已滚出）}，TA 已加载"
echo "  FPGA    ：$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)（密码位流）"
echo "  上电到就绪约 35 秒，全自动。"

# ---------------------------------------------------------------- 二 tour
part "二 走一遍"
MARK=$(wc -l < "$LOG" 2>/dev/null || echo 0)
"$HSM/demo_tour"
TOUR_RC=$?
sleep 1

# ---------------------------------------------------------------- 三 调用链
part "三 刚才那些调用走了哪条路"
echo "  daemon 日志里本次运行的记录（每类调用取第一笔），以及每类调用经过的层："
echo
tail -n +"$((MARK + 1))" "$LOG" 2>/dev/null \
  | grep -E "KEYGEN|ENCAPS|DECAPS|SIGN|VERIFY|SYMKEY|IMPORT_KEY|SYM_CRYPT|SM3|RANDOM|断开" \
  | awk '{ op = $2; if ($2 == "ML-DSA") op = $2 " " $3
           if (seen[op]++) next                      # 每类调用只留第一笔
           i = index($0, "（")                       # 括号里的说明下面的表格讲
           if (i && index($0, "客户端断开") == 0) $0 = substr($0, 1, i - 1)
           print "    " $0 }'
echo
cat <<'EOF'
    生成 / 装载密钥   应用 → daemon → TA（生成/解包）→ PTA → EL3 → FPGA 装载口
    签名 / 解封装     应用 → daemon → TA（重放种子）→ … → FPGA 现展开 → 运算 → 擦
                                   └→ daemon 经 EL3 白名单喂数据、取结果
    加解密 / 杂凑     应用 → daemon → EL3 白名单 → FPGA（按槽号用金库里的密钥）
                      ↑ 这一步不进 TEE：密钥已经在 FPGA 里，每个分组不再走 TA
EOF

# ---------------------------------------------------------------- 四 硬件侧核对
part "四 从硬件侧核对"
if [ -x "$HSM/d7probe" ]; then
    echo "  ① 会话已经结束 —— 直接读 FPGA 的金库状态寄存器："
    "$HSM/d7probe" vmap | sed 's/^/     /'
    echo
    echo "  ② 绕过 TEE，用普通世界直接往 FPGA 的装载口写一把密钥："
    "$HSM/d7probe" | sed 's/^/     /'
    echo
    echo "  ③ 送检位流（SECURE_ONLY=1）下普通世界连读都读不到 —— 证据见"
    echo "     $HSM/d7seclog.txt（这里不现场切位流，那要重配 FPGA）"
else
    echo "  缺 $HSM/d7probe，跳过"
fi

# ---------------------------------------------------------------- 五 怎么调用
part "五 上层怎么调用"
cat <<'EOF'
  ① SDF 风格 C 接口（国密惯用；demo_tour 就是这么写的）

      SDFE_OpenDevice(&dev);  SDFE_OpenSession(dev, &ses);

      SDFE_GenerateKey(ses, 3, 16, blob, &blen);      // TA 生成 SM4 密钥 → 槽 3，交回 blob
      SDFE_EncryptMode(ses, SDFE_ALG_SM4, SDFE_MODE_CBC, 3, iv, in, len, out);
      ...                                             // 下次会话：
      SDFE_LoadKey(ses, 3, blob, blen);               // 同一把密钥重新装进 FPGA

      SDFE_GenerateKeyPair_MLDSA(ses, SDFE_MLDSA_65, pk, &pklen, &h);
      SDFE_Sign_MLDSA(ses, h, msg, msglen, NULL, 0, sig, &siglen);

     头文件 service/sdfe.h，库 libsdfe.a；应用侧不链接任何密码库。

  ② 远程调用（另一台机器，mTLS 双向证书，不用 SSH）

      SDFE_OpenDeviceRemote(&dev, "<板子IP>", 9797, &creds);   // 之后的调用完全一样
      仓库里一条命令：./demo/remote/run.sh <板子IP>

  ③ PKCS#11（跨厂商标准接口）

      模块 demo/bin/pqchsm-pkcs11.so；设 PQCHSM_BACKEND=sdfe 时 ML-KEM 走这台密码机。
      ⚠️ 其余机制在 PKCS#11 侧目前仍是软件后端（PKCS#11 瘦客户端是下一步，FINAL-PLAN PS-13）。
EOF

echo
if [ "$TOUR_RC" -eq 0 ]; then
    echo "${G}${B}演示完成：全部通过。${N}"
else
    echo "${R}${B}演示中有失败项（见上方 ❌）。${N}"
fi

if [ "${1:-}" = "--mgmt" ] && [ -r "$DEMO/functions.sh" ]; then
    part "附：软件密码机的管理功能面（槽位 / 角色 / 备份 / 密钥库）"
    echo "  ⚠️ 这一段是另一个子系统：PS 上的软件密码机（pqchsmd + liboqs），"
    echo "     不经过 TEE 与 FPGA，KDR 是开发用公开常量。只用来展示管理功能。"
    bash "$DEMO/functions.sh"
fi
exit "$TOUR_RC"
