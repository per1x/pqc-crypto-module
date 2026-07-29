#!/usr/bin/env bash
# 热点采样（路线图 §5.2）—— 用 macOS 自带的 sample(1) 做统计式剖析
#
# 为什么用 sample 而不是 perf/callgrind：perf 是 Linux 的；valgrind 在
# arm64 macOS 上支持不完整。sample 是系统自带的采样剖析器，对自己的进程
# 不受 SIP 限制，够用来回答"Keccak 占多少"这一个问题。
#
# 归因的边界（诚实说明）：liboqs 里 Keccak/SHAKE 的符号是**导出**的，
# 所以能直接按符号归因；而 ML-KEM/ML-DSA 内部的 NTT、采样、约减是 static
# 符号，在这个预编译的 .a 里归不到具体函数，只能算进"其余格运算"。
# 要拿到 NTT 的细分，得自己从源码编 liboqs（带 -g），那属于下一步。
#
# 用法：tools/profile.sh [build 目录] [采样秒数]
set -uo pipefail

BUILD="${1:-$(cd "$(dirname "$0")/.." && pwd)/build}"
SECS="${2:-6}"
BENCH="$BUILD/pqchsm-bench"
[ -x "$BENCH" ] || { echo "找不到 $BENCH —— 先 cmake --build build --target pqchsm-bench"; exit 2; }
command -v sample >/dev/null || { echo "SKIP: 本机没有 sample(1)"; exit 0; }

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

profile_one() { # profile_one <alg> <op>
  local alg="$1" op="$2"
  "$BENCH" --loop "$alg" "$op" >/dev/null 2>&1 &
  local pid=$!
  sleep 0.5
  sample "$pid" "$SECS" -mayDie -file "$OUT/$alg-$op.txt" >/dev/null 2>&1
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null

  # sample 的输出里，"Binary Images" 之前是调用树；用叶子样本数按符号归因。
  # 这里用一个粗但稳的办法：统计每个符号出现在样本行里的次数权重。
  python3 - "$OUT/$alg-$op.txt" "$alg" "$op" <<'PY'
import re, sys, collections
path, alg, op = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    txt = open(path, errors="ignore").read()
except OSError:
    sys.exit(0)
# 只取调用树部分
tree = txt.split("Binary Images:")[0]
# 形如 "    1234 Symbol  (in image) ..." —— 取最深层(叶子)的计数
leaf = collections.Counter()
for line in tree.splitlines():
    m = re.match(r"\s*(\d+)\s+(\S.*?)\s+\(in ", line)
    if not m:
        continue
    n, sym = int(m.group(1)), m.group(2).strip()
    leaf[sym] += 0   # 占位，真正的叶子权重下面算
# sample 的树是缩进式；叶子 = 后一行缩进不更深
lines = [l for l in tree.splitlines() if re.match(r"\s*\d+\s+\S", l)]
def indent(l): return len(l) - len(l.lstrip())
total = 0
for i, l in enumerate(lines):
    m = re.match(r"\s*(\d+)\s+(\S.*?)(?:\s+\(in |$)", l)
    if not m:
        continue
    n, sym = int(m.group(1)), m.group(2).strip()
    deeper = i + 1 < len(lines) and indent(lines[i+1]) > indent(l)
    if not deeper:
        leaf[sym] += n
        total += n
if total == 0:
    sys.exit(0)
def bucket(sym):
    s = sym.lower()
    if "keccak" in s or "shake" in s or "sha3" in s or "fips202" in s:
        return "Keccak/SHAKE"
    if "aes" in s or "sha2" in s or "sha256" in s or "sha512" in s:
        return "AES/SHA-2"
    if "randombytes" in s or "rand" in s or "ccrng" in s or "getentropy" in s:
        return "随机源"
    if "memcpy" in s or "memset" in s or "bzero" in s or "malloc" in s or "free" in s:
        return "内存操作"
    return "其余（格运算：NTT/采样/约减等）"
agg = collections.Counter()
for sym, n in leaf.items():
    agg[bucket(sym)] += n
print(f"## {alg} {op}   （样本 {total}）")
for k, v in agg.most_common():
    print(f"   {k:<32} {100.0*v/total:5.1f}%")
# 输出一行机器可读，供 amdahl.py 消费
kec = 100.0 * agg["Keccak/SHAKE"] / total
print(f"@@ {alg} {op} keccak={kec:.1f}")
PY
}

echo "热点采样（每项 ${SECS}s）"
echo
for t in "ML-KEM-768 keygen" "ML-KEM-768 encaps" "ML-KEM-768 decaps" \
         "ML-DSA-65 keygen" "ML-DSA-65 sign" "ML-DSA-65 verify"; do
  # shellcheck disable=SC2086
  profile_one $t
  echo
done
