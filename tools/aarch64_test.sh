#!/usr/bin/env bash
# 在真 aarch64 Linux 上从零构建并跑全套回归
#
# 【为什么这一步值得做】
# 这套代码最终要跑在 Zynq 的 Cortex-A 上（aarch64 Linux）。在开发机（macOS/arm64）
# 上全绿，不代表在目标平台上也全绿：**字节序假设、结构体对齐、long 宽度、
# glibc 与 BSD libc 的行为差异**，只有换平台才会现形。
# 处理器侧固件应先在 PC 或容器中开发验证，这一步就是把这条做法落地。
#
# 【为什么用容器而不是 QEMU】
# 开发机本身就是 arm64，所以 linux/arm64 容器是**原生速度**执行，不是模拟。
# 比 qemu-aarch64 快一个量级，而且跑的是真 glibc + 真 Linux 系统调用。
# （提到的 qemu-aarch64 是给 x86 开发机用的；在 arm64 Mac 上容器更优。）
#
# 用法：tools/aarch64_test.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

command -v docker >/dev/null 2>&1 || { echo "SKIP: 没有 docker/OrbStack"; exit 0; }
docker info >/dev/null 2>&1 || { echo "SKIP: docker 守护进程没起来"; exit 0; }

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

echo "在 linux/arm64 Debian 容器里构建并测试（原生 aarch64，非模拟）"
docker run --rm --platform linux/arm64 \
  "${PROXY_ARGS[@]}" \
  -v "$ROOT":/src:ro \
  -w /work \
  debian:bookworm-slim bash -euo pipefail -c '
set -x
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build git curl libssl-dev python3 ca-certificates >/dev/null
set +x

echo "=== 目标平台信息 ==="
uname -m; uname -s
gcc --version | head -1
echo "sizeof(long)=$(printf "#include <stdio.h>\nint main(){printf(\"%%zu\", sizeof(long));}" > /tmp/a.c && gcc /tmp/a.c -o /tmp/a && /tmp/a)"

echo "=== 构建 liboqs（最小集：本项目用到的 6 个参数集）==="
# 用 codeload 的 tarball 而不是 git clone：单个 HTTP 流，走代理最稳；
# git 的 smart-http 是多次往返，在高延迟链路上更吃亏。
# 代理由外层通过 *_proxy 环境变量注入（见脚本上方说明），curl 自动读取。
mkdir -p /work/liboqs
curl -fsSL --retry 3 --retry-delay 2 \
  https://codeload.github.com/open-quantum-safe/liboqs/tar.gz/refs/tags/0.16.0 \
  | tar xz -C /work/liboqs --strip-components=1
cmake -S /work/liboqs -B /work/liboqs/b -GNinja \
  -DCMAKE_BUILD_TYPE=Release -DOQS_BUILD_ONLY_LIB=ON -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX=/work/oqs \
  -DOQS_MINIMAL_BUILD="KEM_ml_kem_512;KEM_ml_kem_768;KEM_ml_kem_1024;SIG_ml_dsa_44;SIG_ml_dsa_65;SIG_ml_dsa_87" \
  > /dev/null
ninja -C /work/liboqs/b > /dev/null
ninja -C /work/liboqs/b install > /dev/null
echo "liboqs 已装到 /work/oqs"

echo "=== 构建 pqc-hsm ==="
# 源码目录是只读挂载，拷一份出来再构建
cp -r /src /work/pqc-hsm
chmod -R u+w /work/pqc-hsm
rm -rf /work/pqc-hsm/build /work/pqc-hsm/build-asan /work/pqc-hsm/build-tsan /work/pqc-hsm/.venv-rtl
cmake -S /work/pqc-hsm -B /work/pqc-hsm/b -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/work/oqs > /dev/null
# 构建失败必须当场停住。管道加 || true 会把失败吞掉，让后面的 ctest 报一堆
# "Not Run"，真正的原因反而看不见。
if ! ninja -C /work/pqc-hsm/b > /work/build.log 2>&1; then
  echo "构建失败："
  grep -iE "error" /work/build.log | head -30
  exit 1
fi
grep -iE "warning" /work/build.log | head -20 || true
echo "构建完成"

echo "=== 全套回归 ==="
cd /work/pqc-hsm/b
# p11_smoke 需要 pkcs11-tool、rtl_sim 需要 cocotb/iverilog，容器里没装，脚本会自己 SKIP
ctest --output-on-failure 2>&1 | tail -25
'
rc=$?
echo
if [ $rc -eq 0 ]; then
  echo "aarch64 Linux 回归通过"
else
  echo "⚠️ aarch64 Linux 回归失败（退出码 ${rc}）"
fi
exit $rc
