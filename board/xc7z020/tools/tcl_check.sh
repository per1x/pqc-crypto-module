#!/usr/bin/env bash
# Vivado Tcl 脚本的离线检查
#
# 【为什么需要它】Vivado 的 Tcl 脚本只有装了 Vivado 才跑得起来，而本仓库的开发机
# 上没有。一个从未被执行过的脚本，最常见的问题不是 Vivado 命令用错，而是 Tcl
# 层面的低级错误：括号不配对、变量名拼错、分支写反、参数解析漏一个 incr。
# 这些错误不需要 Vivado 就能查出来。
#
# 做法分两步：
#   一、用 `info complete` 检查大括号、方括号与引号是否配对；
#   二、把所有 Vivado 命令替换成桩，在真正的 tclsh 里**把脚本完整执行一遍**。
#      未知命令由 unknown 处理器吞掉并返回合理取值，于是控制流、变量引用、
#      参数解析都会被真实执行到，拼错的变量名会当场报 "can't read"。
#
# 这个检查证明不了"综合能过"，也证明不了 Vivado 命令的参数写对了 ——
# 那两件事只有在装了 Vivado 的机器上跑一遍才知道。它证明的是"脚本本身能跑完"。
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
VIVADO_DIR="$HERE/../vivado"

command -v tclsh >/dev/null 2>&1 || { echo "SKIP: 没有 tclsh"; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

# ---- 第一步：括号与引号配对 ----
cat > "$TMP/complete.tcl" <<'EOF'
set fh [open [lindex $argv 0] r]
set src [read $fh]
close $fh
exit [expr {[info complete $src] ? 0 : 1}]
EOF

echo "Tcl 语法检查（括号与引号配对）"
for f in "$VIVADO_DIR"/*.tcl; do
  if tclsh "$TMP/complete.tcl" "$f"; then
    printf '  ✓ %s\n' "$(basename "$f")"
  else
    printf '  ✗ %s —— 大括号/方括号/引号不配对\n' "$(basename "$f")"
    fail=1
  fi
done

# ---- 第二步：用桩执行 ----
# 桩把 Vivado 命令变成空操作，并对少数几个"返回值会被判断"的命令给出能让脚本
# 走完正常分支的取值。取值本身不重要，重要的是脚本能跑到最后一行。
cat > "$TMP/stub.tcl" <<'EOF'
proc unknown {cmd args} {
    switch -- $cmd {
        get_property {
            set prop [lindex $args 0]
            switch -- $prop {
                PROGRESS  { return "100%" }
                SLACK     { return 1.234 }
                directory { return $::stub_outdir }
                default   { return "" }
            }
        }
        get_board_parts { return {} }
        get_files       { return {} }
        current_project { return "stub_project" }
        current_design  { return "stub_design" }
        default         { return "" }
    }
}
EOF

echo
echo "桩执行（未知命令由 unknown 吞掉，控制流真实执行）"

run_stubbed() { # run_stubbed <脚本> <argv...>
  local script="$1"; shift
  local name; name="$(basename "$script")"
  local driver="$TMP/driver_$name.tcl"
  {
    printf 'set ::stub_outdir %s\n' "$TMP"
    printf 'set argv {%s}\n' "$*"
    printf 'set argc %d\n' "$#"
    cat "$TMP/stub.tcl"
    printf 'source %s\n' "$script"
  } > "$driver"

  local out
  if out="$(cd "$TMP" && tclsh "$driver" 2>&1)"; then
    printf '  ✓ %-22s 执行到结尾\n' "$name"
  else
    printf '  ✗ %-22s\n' "$name"
    printf '%s\n' "$out" | tail -6 | sed 's/^/      /'
    fail=1
  fi
}

# build_bitstream.tcl 在正常流程末尾要找 .bit。桩环境里造出这个产物，
# 让脚本能一路跑到最后一行；否则它会在"找不到 .bit"处正确地提前退出，
# 那一段之后的代码就检查不到了。
touch "$TMP/dummy.xpr"
mkdir -p "$TMP/stub.runs/impl_1"
touch "$TMP/stub.runs/impl_1/stub_wrapper.bit"

run_stubbed "$VIVADO_DIR/create_project.tcl"  -outdir "$TMP/proj" -board pynq_z2
run_stubbed "$VIVADO_DIR/build_bitstream.tcl" -proj "$TMP/dummy.xpr"

echo
if [ "$fail" -eq 0 ]; then
  echo "全部通过"
else
  echo "有脚本未通过"
fi
exit $fail
