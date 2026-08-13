#!/usr/bin/env bash
# build-pdf.sh —— 把说明文档转成 PDF（mermaid 预渲染 + pandoc + 无头 Chrome）
#
# 为什么是这条链、而不是 pandoc→LaTeX：
#  · LaTeX 要装整套 TeX + 配中文字体，而 macOS 上 Chrome 直接就有系统 CJK 字体，
#    中文零配置不乱码；
#  · mermaid 先用 mmdc 渲染成 SVG 再嵌，PDF 里是矢量图，放大不糊；
#  · Chrome 的分页规则（表头跨页重复、单行不断开）比 LaTeX 好调。
#
# 前置：pandoc、node/npm、Google Chrome。mmdc 用 npx 按需取，不常驻。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/docs/密码机原型-说明文档.md"
OUT="$ROOT/docs/密码机原型-说明文档.pdf"
CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
cp "$ROOT/tools/pdf/"{style.css,mconf.json,pptr.json} "$W/"

# 1. 抽出 mermaid，渲染成 SVG
python3 - "$SRC" "$W/arch.mmd" <<'PY'
import re, sys
s = open(sys.argv[1], encoding='utf-8').read()
open(sys.argv[2], 'w', encoding='utf-8').write(
    re.search(r'```mermaid\n(.*?)```', s, re.S).group(1))
PY
export PUPPETEER_SKIP_CHROMIUM_DOWNLOAD=true PUPPETEER_SKIP_DOWNLOAD=true
npx -y @mermaid-js/mermaid-cli@11 -i "$W/arch.mmd" -o "$W/arch.svg" \
    -p "$W/pptr.json" -c "$W/mconf.json" -b white

# mmdc 出来的 SVG 是 width="100%" 且没有 height，浏览器算不出固有尺寸，
# 嵌进 <img> 会缩成一小块。按 viewBox 把宽高写死。
python3 - "$W/arch.svg" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p, encoding='utf-8').read()
vb = re.search(r'viewBox="0 0 ([\d.]+) ([\d.]+)"', s)
s = s.replace('width="100%"',
              'width="%d" height="%d"' % (round(float(vb.group(1))),
                                          round(float(vb.group(2)))), 1)
s = re.sub(r'style="max-width: [\d.]+px; ', 'style="', s, count=1)
open(p, 'w', encoding='utf-8').write(s)
PY

# 2. 把 mermaid 块换成图片引用
python3 - "$SRC" "$W/doc.md" <<'PY'
import re, sys
s = open(sys.argv[1], encoding='utf-8').read()
s = re.sub(r'```mermaid\n.*?```',
           '![后量子密码机原型总体架构](arch.svg)', s, count=1, flags=re.S)
open(sys.argv[2], 'w', encoding='utf-8').write(s)
PY

# 3. Markdown → 自包含 HTML（SVG 以 data URI 内嵌）
#    不传 --metadata title：文档自己有 H1，传了会出两个标题。
( cd "$W" && pandoc doc.md -o doc.html --standalone --embed-resources \
      --css=style.css --syntax-highlighting=tango )

# 4. HTML → PDF
"$CHROME" --headless --disable-gpu --no-sandbox --no-pdf-header-footer \
          --print-to-pdf="$W/doc.pdf" --virtual-time-budget=20000 \
          "file://$W/doc.html" 2>/dev/null
cp "$W/doc.pdf" "$OUT"
cp "$W/arch.svg" "$ROOT/docs/密码机原型-架构图.svg"
echo "产物：$OUT"
python3 -c "
import sys; d=open('$OUT','rb').read()
print('  %.1f KB，%d 页' % (len(d)/1024, d.count(b'/Type /Page')-d.count(b'/Type /Pages')))"
