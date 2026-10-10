#!/usr/bin/env python3
"""把业务流程演示（board/demo/business_demo.sh）的原始输出转成一页 HTML 报告。

    ssh root@<板子> 'sh /media/sd-mmcblk1p2/demo/run.sh' > demo.txt
    python3 tools/demo_to_html.py demo.txt demo.html
    python3 tools/demo_to_html.py cli.txt http.txt demo.html     # 多份输出合成一页

输入保留 ANSI 颜色码更好（用来区分「说明」与「输出」）；没有也能转。
页面完全自包含（无外部脚本 / 字体 / 图片），可以直接发给别人打开。
"""
import html
import re
import sys
from datetime import datetime

ANSI = re.compile(r"\x1b\[[0-9;]*m")
DIM = "\x1b[2m"

TAGS = {
    "[TEE]": ("tee", "TEE", "OP-TEE 里的 TA"),
    "[TEE→PL]": ("bridge", "TEE→PL", "安全通路写进 FPGA"),
    "[PL]": ("pl", "PL", "FPGA 密码核"),
    "[NW]": ("nw", "NW", "普通世界 daemon"),
    "[HTTP]": ("nw", "HTTP", "本机 HTTP 网关收到的请求"),
}


def parse(raw):
    """→ (header_lines, parts)；part = {title, steps}；step = {title, items}"""
    header, parts = [], []
    part = step = None
    for line in raw.splitlines():
        plain = ANSI.sub("", line)
        s = plain.strip()
        if not s:
            continue
        if s.startswith("══════"):
            part = {"title": s.strip("═ ").strip(), "steps": []}
            parts.append(part)
            step = None
            continue
        if part is None:
            header.append(s)
            continue
        if s.startswith("▶ "):
            step = {"title": s[2:], "items": []}
            part["steps"].append(step)
            continue
        if step is None:
            step = {"title": "", "items": []}
            part["steps"].append(step)
        if s.startswith("$ "):
            step["items"].append({"t": "cmd", "text": s[2:], "out": [], "log": []})
            continue
        if s.startswith("│ "):
            body = s[2:]
            for tag, meta in TAGS.items():
                if body.startswith(tag):
                    entry = {"cls": meta[0], "tag": meta[1],
                             "text": body[len(tag):].strip()}
                    if step["items"] and step["items"][-1]["t"] == "cmd":
                        step["items"][-1]["log"].append(entry)
                    break
            continue
        if line.lstrip(" ").startswith(DIM):
            step["items"].append({"t": "note", "text": s})
            continue
        if s.startswith("演示结束"):
            continue
        if step["items"] and step["items"][-1]["t"] == "cmd":
            step["items"][-1]["out"].append(plain[2:] if plain.startswith("  ") else plain)
        else:
            step["items"].append({"t": "note", "text": s})
    return header, parts


def out_line(l):
    e = html.escape(l)
    st = l.strip()
    if st.startswith("✓") or st.startswith("PASS") or "逐字节一致" in st or re.match(r"HTTP 2\d\d", st):
        return f'<span class="ok">{e}</span>'
    if st.startswith("✗") or st.startswith("FAIL") or re.match(r"HTTP [45]\d\d", st):
        return f'<span class="bad">{e}</span>'
    return e


def render(header, parts, src_name):
    n_cmd = n_tee = n_pl = n_bridge = n_rej = 0
    for p in parts:
        for st in p["steps"]:
            for it in st["items"]:
                if it["t"] != "cmd":
                    continue
                n_cmd += 1
                n_rej += any(o.strip().startswith("✗") for o in it["out"])
                for lg in it["log"]:
                    if lg["text"].startswith("会话结束"):
                        continue
                    n_tee += lg["cls"] == "tee"
                    n_pl += lg["cls"] == "pl"
                    n_bridge += lg["cls"] == "bridge"

    toc = "".join(f'<a href="#p{i}">{html.escape(p["title"])}</a>' for i, p in enumerate(parts))
    body = []
    for i, p in enumerate(parts):
        body.append(f'<section class="part" id="p{i}"><h2>{html.escape(p["title"])}</h2>')
        for st in p["steps"]:
            body.append('<div class="step">')
            if st["title"]:
                body.append(f'<h3>{html.escape(st["title"])}</h3>')
            for it in st["items"]:
                if it["t"] == "note":
                    body.append(f'<p class="note">{html.escape(it["text"])}</p>')
                    continue
                body.append('<div class="cmd">')
                body.append(f'<div class="term"><span class="prompt">$</span> '
                            f'<code>{html.escape(it["text"])}</code></div>')
                if it["out"]:
                    body.append('<pre class="out">' + "\n".join(out_line(o) for o in it["out"])
                                + "</pre>")
                logs = [lg for lg in it["log"] if not lg["text"].startswith("会话结束")]
                ends = [lg for lg in it["log"] if lg["text"].startswith("会话结束")]
                if logs or ends:
                    body.append('<ol class="trace">')
                    for lg in logs:
                        body.append(f'<li class="{lg["cls"]}"><span class="badge">{lg["tag"]}</span>'
                                    f'<span>{html.escape(lg["text"])}</span></li>')
                    for lg in ends:
                        body.append(f'<li class="pl end"><span class="badge">PL</span>'
                                    f'<span>{html.escape(lg["text"])}</span></li>')
                    body.append("</ol>")
                body.append("</div>")
            body.append("</div>")
        body.append("</section>")

    legend = "".join(f'<span class="chip {c}"><span class="badge">{t}</span>{d}</span>'
                     for c, t, d in TAGS.values())
    stamp = datetime.now().strftime("%Y-%m-%d %H:%M")
    return TEMPLATE.format(
        legend=legend, toc=toc, body="\n".join(body),
        n_cmd=n_cmd, n_tee=n_tee, n_pl=n_pl, n_bridge=n_bridge, n_rej=n_rej,
        src=html.escape(src_name), stamp=stamp)


TEMPLATE = """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>密码机业务演示</title>
<style>
:root {{
  --bg:#f6f7f9; --card:#ffffff; --ink:#1d2330; --muted:#667085; --line:#e4e7ec;
  --term:#0f1520; --term-ink:#d8dee9; --prompt:#7dd3a8;
  --tee:#1f8a5b; --tee-bg:#e7f6ee; --bridge:#b26a00; --bridge-bg:#fff4e0;
  --pl:#2563c9; --pl-bg:#e8f0fd; --nw:#6b7280; --nw-bg:#f0f1f3;
  --ok:#14804a; --bad:#c2372d;
}}
@media (prefers-color-scheme: dark) {{
  :root {{
    --bg:#0e1117; --card:#161b24; --ink:#e6e9ef; --muted:#98a2b3; --line:#283040;
    --term:#0a0e15; --term-ink:#d8dee9;
    --tee:#5fd39a; --tee-bg:#13291f; --bridge:#f0b45a; --bridge-bg:#2c2214;
    --pl:#7aa7ff; --pl-bg:#15213a; --nw:#a0a7b4; --nw-bg:#1d222c;
    --ok:#5fd39a; --bad:#ff7b72;
  }}
}}
* {{ box-sizing:border-box }}
body {{ margin:0; background:var(--bg); color:var(--ink);
  font:15px/1.6 -apple-system,BlinkMacSystemFont,"PingFang SC","Hiragino Sans GB","Microsoft YaHei","Segoe UI",sans-serif; }}
.wrap {{ max-width:1040px; margin:0 auto; padding:32px 20px 64px }}
header h1 {{ margin:0 0 4px; font-size:28px; letter-spacing:.5px }}
header .sub {{ color:var(--muted); margin:0 0 20px }}
.arch {{ display:grid; grid-template-columns:repeat(3,1fr); gap:12px; margin:0 0 20px }}
.arch div {{ background:var(--card); border:1px solid var(--line); border-radius:12px; padding:14px 16px }}
.arch b {{ display:block; margin-bottom:4px }}
.arch .t b {{ color:var(--tee) }} .arch .p b {{ color:var(--pl) }} .arch .n b {{ color:var(--nw) }}
.arch span {{ color:var(--muted); font-size:13.5px }}
.stats {{ display:grid; grid-template-columns:repeat(5,1fr); gap:12px; margin:0 0 20px }}
.stat {{ background:var(--card); border:1px solid var(--line); border-radius:12px; padding:12px 16px }}
.stat .v {{ font-size:26px; font-weight:700 }}
.stat .k {{ color:var(--muted); font-size:13px }}
.stat.tee .v {{ color:var(--tee) }} .stat.pl .v {{ color:var(--pl) }}
.stat.bridge .v {{ color:var(--bridge) }} .stat.rej .v {{ color:var(--bad) }}
.legend {{ display:flex; flex-wrap:wrap; gap:10px; margin:0 0 16px }}
.chip {{ display:inline-flex; align-items:center; gap:8px; font-size:13.5px; color:var(--muted) }}
nav {{ position:sticky; top:0; z-index:5; background:var(--bg); padding:10px 0; margin-bottom:8px;
  display:flex; flex-wrap:wrap; gap:6px; border-bottom:1px solid var(--line) }}
nav a {{ color:var(--ink); text-decoration:none; font-size:13.5px; padding:4px 10px;
  border:1px solid var(--line); border-radius:999px; background:var(--card) }}
nav a:hover {{ border-color:var(--pl) }}
.part {{ margin-top:28px }}
.part h2 {{ font-size:20px; margin:0 0 12px; padding-left:10px; border-left:4px solid var(--pl) }}
.step {{ background:var(--card); border:1px solid var(--line); border-radius:14px;
  padding:16px 18px; margin:0 0 14px }}
.step h3 {{ margin:0 0 10px; font-size:16px }}
.cmd + .cmd {{ margin-top:14px }}
.term {{ background:var(--term); color:var(--term-ink); border-radius:10px 10px 0 0;
  padding:10px 14px; font:13.5px/1.5 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
  overflow-x:auto; white-space:pre }}
.term .prompt {{ color:var(--prompt); font-weight:700 }}
.out {{ margin:0; background:var(--term); color:var(--term-ink); border-top:1px solid #1f2937;
  padding:10px 14px; font:13px/1.55 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
  white-space:pre-wrap; word-break:break-all; border-radius:0 0 10px 10px }}
.term:last-child {{ border-radius:10px }}
.out .ok {{ color:#6ee7a8 }} .out .bad {{ color:#ff8a80 }}
.trace {{ list-style:none; margin:10px 0 0; padding:0; display:flex; flex-direction:column; gap:6px }}
.trace li {{ display:flex; gap:10px; align-items:flex-start; padding:7px 10px; border-radius:8px;
  font-size:13.5px }}
.trace li.tee {{ background:var(--tee-bg) }} .trace li.bridge {{ background:var(--bridge-bg) }}
.trace li.pl {{ background:var(--pl-bg) }} .trace li.nw {{ background:var(--nw-bg) }}
.trace li.end {{ opacity:.6 }}
.badge {{ flex:none; display:inline-block; min-width:64px; text-align:center; font-weight:700;
  font-size:12px; padding:1px 8px; border-radius:6px; color:#fff;
  font-family:ui-monospace,SFMono-Regular,Menlo,monospace }}
.tee .badge {{ background:var(--tee) }} .bridge .badge {{ background:var(--bridge) }}
.pl .badge {{ background:var(--pl) }} .nw .badge {{ background:var(--nw) }}
.note {{ margin:10px 0 0; color:var(--muted); font-size:14px; padding-left:12px;
  border-left:3px solid var(--line) }}
footer {{ margin-top:36px; color:var(--muted); font-size:13px }}
@media (max-width:720px) {{
  .arch {{ grid-template-columns:1fr }} .stats {{ grid-template-columns:repeat(2,1fr) }}
  .badge {{ min-width:52px }}
}}
</style></head>
<body><div class="wrap">
<header>
  <h1>后量子密码机 · 业务流程演示</h1>
  <p class="sub">AXU3EGB（Zynq UltraScale+）· 密钥在 TEE 里管 · 运算在 FPGA 里做 · 应用经命令行 / HTTP 接口调用</p>
</header>
<div class="arch">
  <div class="t"><b>TEE（OP-TEE TA）</b><span>生成种子与对称密钥（熵 = OP-TEE PRNG ⊕ PL TRNG），用 KEK 包成 blob；需要时解开、经安全通路装进 FPGA。KEK 不出 TA。</span></div>
  <div class="p"><b>FPGA（PL）</b><span>ML-KEM、ML-DSA、AES / SM4、SM3、真随机数。私钥每次现展开、算完由硬件擦；对称密钥在金库里无读出路径。</span></div>
  <div class="n"><b>普通世界（Linux）</b><span>daemon 调度与接口（SDF / PKCS#11 / 远程 mTLS）。手里只有句柄与密文 blob，没有任何明文密钥。</span></div>
</div>
<div class="stats">
  <div class="stat"><div class="v">{n_cmd}</div><div class="k">条命令</div></div>
  <div class="stat tee"><div class="v">{n_tee}</div><div class="k">步在 TEE 里执行</div></div>
  <div class="stat bridge"><div class="v">{n_bridge}</div><div class="k">次经安全通路写进 FPGA</div></div>
  <div class="stat pl"><div class="v">{n_pl}</div><div class="k">步在 FPGA 里执行</div></div>
  <div class="stat rej"><div class="v">{n_rej}</div><div class="k">次按预期被拒绝</div></div>
</div>
<div class="legend">{legend}</div>
<nav>{toc}</nav>
{body}
<footer>由 <code>tools/demo_to_html.py</code> 从板上实测输出（{src}）生成 · {stamp}<br>
每条执行记录都是 daemon 在对应那一层调用成功返回之后写下的，不是事先写好的旁白。
"按预期被拒绝"是演示里故意做的反例：伪造合同验签、销毁后再用、篡改备份包、篡改密钥文件。</footer>
</div></body></html>
"""


def main():
    if len(sys.argv) < 3:
        sys.exit("用法：demo_to_html.py <演示输出.txt> [更多输出.txt …] <输出.html>")
    ins, out = sys.argv[1:-1], sys.argv[-1]
    header, parts = [], []
    for f in ins:          # 每份各自解析再拼接：板上命令行演示 + HTTP 演示可以合成一页
        h, p = parse(open(f, encoding="utf-8").read())
        header += h
        parts += p
    if not parts:
        sys.exit("没认出任何段落 —— 输入不是 business_demo.sh / http_demo.sh 的输出？")
    open(out, "w", encoding="utf-8").write(
        render(header, parts, "、".join(f.split("/")[-1] for f in ins)))
    print(f"已生成 {out}：{len(parts)} 段")


if __name__ == "__main__":
    main()
