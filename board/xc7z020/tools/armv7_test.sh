#!/usr/bin/env bash
# 交叉编译到 armv7（Zynq-7000 / Cortex-A9）并在 QEMU 下跑全套回归
#
# 【为什么这一步值得做】
# 目标板 XC7Z020 的 PS 是 32 位的 Cortex-A9（armv7l，硬浮点 gnueabihf），
# 而开发机是 64 位的 arm64。两者的数据模型不同：armv7l 是 ILP32，
# long 与指针都是 4 字节；aarch64 是 LP64 的 8 字节。凡是"把 64 位值塞进
# long / unsigned long"的假设，在开发机上全绿，换到目标板才会现形 ——
# PKCS#11 尤其危险，因为 CK_ULONG 的定义就是 unsigned long。
#
# 【为什么是交叉编译 + qemu-arm，而不是原生容器】
# tools/aarch64_test.sh 能用原生 linux/arm64 容器，是因为开发机本身就是 arm64。
# 但 Apple Silicon **不支持 AArch32 执行状态**，32 位 ARM 代码在这台机器上
# 只能靠模拟。整个工程连 liboqs 一起放进模拟器里编译要慢一个数量级，
# 所以这里拆开：在原生 arm64 容器里用 arm-linux-gnueabihf 工具链交叉编译，
# 只把**跑测试**这一步交给 qemu-arm。
#
# 【依赖来自 Debian multiarch】
# dpkg --add-architecture armhf 之后，libssl-dev:armhf 的库落在
# /usr/lib/arm-linux-gnueabihf，头文件与宿主共用 /usr/include。
# 工具链文件靠 CMAKE_LIBRARY_ARCHITECTURE 让 find_library 先命中 armhf 那份。
# liboqs 没有 armhf 的 Debian 包，用同一个工具链文件从源码交叉编译。
#
# 用法：board/xc7z020/tools/armv7_test.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
TOOLCHAIN_REL="board/xc7z020/cmake/armv7-linux-gnueabihf.cmake"

command -v docker >/dev/null 2>&1 || { echo "SKIP: 没有 docker/OrbStack"; exit 0; }
docker info >/dev/null 2>&1 || { echo "SKIP: docker 守护进程没起来"; exit 0; }
[ -f "$ROOT/$TOOLCHAIN_REL" ] || { echo "SKIP: 找不到 $TOOLCHAIN_REL"; exit 0; }

# 容器不继承宿主的 HTTP_PROXY，且容器内的 127.0.0.1 指向容器自身，因此在受限
# 网络下容器里的下载（Debian 源与 liboqs 源码包）都会极慢。这里在宿主代理端口
# 确实处于监听状态时把它透进容器，否则保持直连 —— 没有代理的机器上脚本照常工作。
# 代理端口可用 PQCHSM_PROXY_PORT 覆盖。
PROXY_PORT="${PQCHSM_PROXY_PORT:-6152}"
PROXY_ARGS=()
if command -v nc >/dev/null 2>&1 && nc -z 127.0.0.1 "$PROXY_PORT" 2>/dev/null; then
  PROXY_URL="http://host.docker.internal:${PROXY_PORT}"
  PROXY_ARGS=(--add-host=host.docker.internal:host-gateway
              -e "http_proxy=$PROXY_URL"  -e "https_proxy=$PROXY_URL"
              -e "HTTP_PROXY=$PROXY_URL"  -e "HTTPS_PROXY=$PROXY_URL"
              -e "no_proxy=localhost,127.0.0.1")
  echo "宿主 127.0.0.1:${PROXY_PORT} 在监听 -> 容器整体走宿主代理"
else
  echo "宿主 127.0.0.1:${PROXY_PORT} 没在监听 -> 容器直连"
  echo "  （受限网络下 apt 与 liboqs 都会很慢；有代理时设 PQCHSM_PROXY_PORT 指过去）"
fi

echo "在 linux/arm64 Debian 容器里交叉编译到 armv7，测试交给 qemu-arm"
docker run --rm --platform linux/arm64 \
  "${PROXY_ARGS[@]}" \
  -v "$ROOT":/src:ro \
  -w /work \
  debian:bookworm-slim bash -euo pipefail -c '
set -x
dpkg --add-architecture armhf
apt-get update -qq
# crossbuild-essential-armhf 带来 arm-linux-gnueabihf 工具链与 armhf 的 libc；
# qemu-user-static 提供 qemu-arm-static，用来执行交叉出来的 32 位二进制。
apt-get install -y -qq --no-install-recommends \
    crossbuild-essential-armhf qemu-user-static \
    cmake ninja-build curl ca-certificates python3 file pkg-config \
    libssl-dev:armhf >/dev/null
set +x

TC=/src/board/xc7z020/cmake/armv7-linux-gnueabihf.cmake

echo "=== 目标平台信息 ==="
arm-linux-gnueabihf-gcc --version | head -1
qemu-arm-static --version | head -1
cat > /work/widths.c <<CEOF
#include <stdio.h>
#include <time.h>
#include <sys/types.h>
int main(void) {
    printf("uname 目标=armv7l  long=%zu ptr=%zu size_t=%zu off_t=%zu time_t=%zu\n",
           sizeof(long), sizeof(void *), sizeof(size_t), sizeof(off_t), sizeof(time_t));
    return 0;
}
CEOF
# 与工程一致地带上 _FILE_OFFSET_BITS / _TIME_BITS，报出的宽度才是真实构建的宽度
arm-linux-gnueabihf-gcc -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 \
    /work/widths.c -o /work/widths
qemu-arm-static /work/widths

# binfmt_misc 里注册了 qemu-arm 时，交叉产物可以直接执行，ctest 里那些
# 靠 shell 驱动二进制的用例（cli_smoke / e2e_p11 等）才跑得起来。
# 没有注册也不致命：工具链文件设了 CMAKE_CROSSCOMPILING_EMULATOR，
# ctest 仍然能跑直接由可执行目标构成的用例。
if /work/widths >/dev/null 2>&1; then
  BINFMT=1
  echo "binfmt_misc 已注册 qemu-arm -> armv7 二进制可直接执行，全部用例都能跑"
else
  BINFMT=0
  echo "binfmt_misc 没有 qemu-arm -> 只跑可执行目标构成的用例，脚本驱动的用例排除"
fi

echo "=== 交叉编译 liboqs（最小集：本项目用到的 6 个参数集）==="
# 用 codeload 的 tarball 而不是 git clone：单个 HTTP 流，走代理最稳；
# git 的 smart-http 是多次往返，在高延迟链路上更吃亏。
mkdir -p /work/liboqs
curl -fsSL --retry 3 --retry-delay 2 \
  https://codeload.github.com/open-quantum-safe/liboqs/tar.gz/refs/tags/0.16.0 \
  | tar xz -C /work/liboqs --strip-components=1
cmake -S /work/liboqs -B /work/liboqs/b -GNinja \
  -DCMAKE_TOOLCHAIN_FILE="$TC" \
  -DCMAKE_BUILD_TYPE=Release -DOQS_BUILD_ONLY_LIB=ON -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX=/work/oqs \
  -DOQS_MINIMAL_BUILD="KEM_ml_kem_512;KEM_ml_kem_768;KEM_ml_kem_1024;SIG_ml_dsa_44;SIG_ml_dsa_65;SIG_ml_dsa_87" \
  > /work/oqs-cmake.log 2>&1 \
  || { echo "liboqs 配置失败："; tail -20 /work/oqs-cmake.log; exit 1; }
ninja -C /work/liboqs/b > /work/oqs-build.log 2>&1 \
  || { echo "liboqs 构建失败："; grep -iE "error" /work/oqs-build.log | head -20; exit 1; }
ninja -C /work/liboqs/b install > /dev/null
file /work/oqs/lib/liboqs.a | head -1

echo "=== 交叉编译 pqc-hsm ==="
# 源码目录是只读挂载，拷一份出来再构建
cp -r /src /work/pqc-hsm
chmod -R u+w /work/pqc-hsm
rm -rf /work/pqc-hsm/build /work/pqc-hsm/build-asan /work/pqc-hsm/build-tsan \
       /work/pqc-hsm/.venv-rtl /work/pqc-hsm/.venv-p11
cmake -S /work/pqc-hsm -B /work/pqc-hsm/b -GNinja \
  -DCMAKE_TOOLCHAIN_FILE="$TC" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/work/oqs > /work/cmake.log 2>&1 \
  || { echo "配置失败："; tail -25 /work/cmake.log; exit 1; }
grep -E "liboqs:|OpenSSL|time_t" /work/cmake.log || true
# 构建失败必须当场停住。管道加 || true 会把失败吞掉，让后面的 ctest 报一堆
# "Not Run"，真正的原因反而看不见。
if ! ninja -C /work/pqc-hsm/b > /work/build.log 2>&1; then
  echo "构建失败："
  grep -iE "error" /work/build.log | head -30
  exit 1
fi
# 32 位下最容易出问题的几类告警单独列出来：移位越界、格式串、宽度截断。
echo "--- 与宽度相关的告警（应为空）---"
grep -iE "shift-count-overflow|shift-overflow|format=|format-overflow|int-to-pointer|pointer-to-int" \
     /work/build.log | head -20 || true
grep -icE "warning" /work/build.log | sed "s/^/告警总数：/"
file /work/pqc-hsm/b/test_p11 | head -1
echo "构建完成"

echo "=== 全套回归（armv7 二进制，qemu-arm 执行）==="
cd /work/pqc-hsm/b
# p11_smoke 需要 pkcs11-tool、rtl_sim 需要 cocotb/iverilog，容器里没装，脚本会自己 SKIP
if [ "$BINFMT" = "1" ]; then
  ctest --output-on-failure 2>&1 | tail -30
else
  # 这几条是 shell 脚本驱动的：脚本自己 exec armv7 二进制，
  # 拿不到 CMAKE_CROSSCOMPILING_EMULATOR 的前缀，没有 binfmt 就跑不了。
  ctest --output-on-failure -E "cli_smoke|e2e_p11|p11_demos|p11_smoke|rtl_sim" 2>&1 | tail -30
fi
'
rc=$?
echo
if [ $rc -eq 0 ]; then
  echo "armv7 交叉编译与 QEMU 回归通过"
else
  echo "⚠️ armv7 交叉编译或 QEMU 回归失败（退出码 $rc）"
fi
exit $rc
