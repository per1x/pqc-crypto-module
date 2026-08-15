#!/bin/sh
# build_client.sh —— 从仓库源码构建远程客户端
#
# ============================================================================
# 【为什么要有这个脚本】
# ============================================================================
# 远程演示要在**另一台机器**上跑一个客户端连密码机。那个客户端就是
# service/ 下的 sdf_demo + libsdfe，本身与硬件无关（一行密码运算都不做，
# 只把 SDFE_* 调用翻译成一条到 daemon 的请求）。
#
# 但它一直靠"临时 cc 一下"存在，产物放在 /tmp，一清就没 —— 于是每次演示前
# 都要重新想一遍怎么编。这个脚本把它固定下来：一条命令，从仓库源码出客户端，
# 谁在哪台机器上都能复现。
#
# 客户端**不依赖 liboqs、不依赖 OpenSSL** —— 它不做密码运算，只有 socket。
# 所以 `cc` 就够，不需要那套构建环境。这也是把它和主 CMake 工程分开的理由：
# 演示的人手上未必有 liboqs。
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SVC="$HERE/../../service"
OUT="${1:-$HERE/sdf_demo}"

cc -O2 -Wall -I "$SVC" -o "$OUT" \
   "$SVC/sdf_demo.c" "$SVC/libsdfe.c"
echo "SDF 客户端已构建：$OUT"

# PKCS#11 演示：标准接口 → FPGA。也不依赖 liboqs/OpenSSL（纯 dlopen）。
ROOT="$HERE/../.."
P11OUT="$(dirname "$OUT")/p11_hw_demo"
cc -O2 -Wall -I "$ROOT/third_party/pkcs11-v3.2" -I "$ROOT/src/p11" \
   -o "$P11OUT" "$HERE/p11_hw_demo.c"
echo "PKCS#11 演示已构建：$P11OUT"
echo
echo
echo "本机连（板子上有 daemon 的 UNIX socket）："
echo "    $OUT"
echo "远程连（另一台机器，先从板子拿口令）："
echo "    TOK=\$(ssh root@192.168.50.175 cat /media/sd-mmcblk1p2/hsm/hsm_token)"
echo "    $OUT 192.168.50.175 \"\$TOK\""
