#!/usr/bin/env bash
# tee/tests 的原生构建与运行 —— 把 TA 侧的密码原语纳入 ctest
#
# 【为什么值得接进来】
# TA 的 KMAC-256、PWRP 包裹、ML-KEM/ML-DSA 都是**另一份实现**（TA 里没有
# OpenSSL，海绵是自带的）。它们与普通世界那份的一致性不是自动成立的，
# 而 tee/tests 正是把两边对拍的地方（含 200 轮随机对拍）。
# 以前它只能手工 `make && ./run_tests`，于是"跑没跑过"取决于有没有人记得。
#
# 缺 OpenSSL 开发头时 SKIP（退出 0），与本仓库其它可选依赖同一个口径 ——
# 让一台没装依赖的机器把整个回归判成失败，只会让人学会忽略失败。
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/tee/tests"

command -v make >/dev/null 2>&1 || { echo "SKIP: 没有 make"; exit 0; }

# OpenSSL 只是对拍参照，不是被测代码。问 pkg-config，问不到就试裸编。
if ! pkg-config --exists libcrypto 2>/dev/null; then
  if ! printf '#include <openssl/evp.h>\nint main(void){return 0;}\n' \
       | ${CC:-cc} -x c - -o /dev/null -lcrypto >/dev/null 2>&1; then
    echo "SKIP: 找不到 OpenSSL 开发头（tee/tests 用它做对拍参照）"
    exit 0
  fi
fi

cd "$DIR" || { echo "SKIP: 没有 tee/tests"; exit 0; }
make -s clean >/dev/null 2>&1
if ! make -s; then
  echo "✗ tee/tests 编译失败"
  exit 1
fi
./run_tests
rc=$?
make -s clean >/dev/null 2>&1
exit $rc
