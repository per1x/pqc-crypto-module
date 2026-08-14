#!/usr/bin/env bash
# build-pdf.sh —— 生成《设计与验证参考》PDF
#
# 单一事实来源：PDF 由 docs/ 下的中文文档拼出来，**不另存一份 Markdown**。
# 这样文档改了 PDF 重出就是最新的，不会出现两份内容互相漂移。
#
# 为什么是 pandoc → 无头 Chrome，而不是 pandoc → LaTeX：
#  · LaTeX 要装整套 TeX + 配中文字体，而 macOS 上 Chrome 直接就有系统 CJK
#    字体，中文零配置不乱码；
#  · Chrome 的分页规则（表头跨页重复、单行不断开）比 LaTeX 好调。
#
# 前置：pandoc、Google Chrome。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/docs/reference/design-validation.zh-CN.pdf"
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

# 章节顺序 = 读者该按什么顺序读：先知道它是什么，再看接口，再看寄存器，
# 最后看边界与证据。
CHAPTERS=(
    ARCHITECTURE.zh-CN.md
    API.zh-CN.md
    REGISTERS.zh-CN.md
    SECURITY.zh-CN.md
    TESTING.zh-CN.md
)

command -v pandoc >/dev/null 2>&1 || { echo "缺 pandoc（brew install pandoc）"; exit 1; }
[ -x "$CHROME" ] || { echo "缺 Google Chrome：$CHROME"; exit 1; }

W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
cp "$ROOT/tools/pdf/style.css" "$W/"
cp "$ROOT/docs/diagrams/architecture.svg" "$W/"

{
    cat <<'MD'
---
title: "后量子密码机原型 —— 设计与验证参考"
subtitle: "ZU3EG / XCZU3EG · 分支 zu3eg-fpga-crypto"
lang: zh-CN
---

> 本文由仓库 `docs/` 下的中文文档拼合生成，内容与仓库保持一致。
> 文中所有测量数字均来自**真实硬件运行**，不是仿真、不是估算。
> 原始日志见仓库 `board/logs/`。

![总体架构](architecture.svg)

MD
    for c in "${CHAPTERS[@]}"; do
        src="$ROOT/docs/$c"
        [ -f "$src" ] || { echo "缺章节：$src" >&2; exit 1; }
        printf '\n\\newpage\n\n'
        # 去掉第一行的语种切换链接 —— 那是给网页看的，PDF 里是噪声。
        sed '1{/^\[English\]/d;}' "$src"
        printf '\n'
    done
} > "$W/doc.md"

# Markdown → 自包含 HTML（SVG 以 data URI 内嵌）
( cd "$W" && pandoc doc.md -o doc.html --standalone --embed-resources \
      --toc --toc-depth=2 --css=style.css --syntax-highlighting=tango )

"$CHROME" --headless --disable-gpu --no-sandbox --no-pdf-header-footer \
          --print-to-pdf="$W/doc.pdf" --virtual-time-budget=20000 \
          "file://$W/doc.html" 2>/dev/null

cp "$W/doc.pdf" "$OUT"
echo "产物：$OUT"
python3 -c "
d = open('$OUT','rb').read()
print('  %.1f KB，%d 页' % (len(d)/1024, d.count(b'/Type /Page') - d.count(b'/Type /Pages')))"
