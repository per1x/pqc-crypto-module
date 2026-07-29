#!/usr/bin/env python3
"""常量时间源码审计：扫描 src/ 下与密钥材料相关的代码路径，找出秘密相关的
分支、内存下标、除法取模，以及用非常量时间函数比较秘密数据的地方。

【为什么要有这个工具】
常量时间是一条无法靠"写的时候小心"维持的性质：它对每一次改动都敏感，而破坏
它的写法（`if (secret[i])`、`memcmp(tag, want, n)`、`table[secret_byte]`）在
功能测试里完全看不出来。把判据固化成脚本，每次回归都重扫一遍，才谈得上是可
复现的结论而不是口头断言。

【扫描的四条规则】
  R1 比较    非常量时间的比较函数（memcmp/strcmp/strncmp/bcmp）作用于秘密数据
  R2 分支    if / while / switch / ?: 的判据里出现秘密数据
  R3 下标    数组下标表达式由秘密数据派生（表查找会在 D-cache 上留痕迹）
  R4 除法    除法或取模的除数由秘密数据派生（除法器耗时与操作数相关）

【秘密的判定】
按标识符命名判定，规则见 SECRET_PATTERNS / NON_SECRET_PATTERNS。命名约定在本
仓库是稳定的（sk / seed / pin / kek / bek / rmk / cek / ss / verifier / tag …），
按名字判定会有误报，但不会漏掉整类数据 —— 审计工具宁可误报。

【白名单】
误报和"已审阅、确实安全"的地方用注释标记声明：

    /* 常量时间：判据只与公开的分片索引有关 */
    if (xs[i] == xs[t]) { ... }

标记写在同一行、或紧贴在上方的注释块里都算数；理由跟在冒号之后、写在同一行内，
不能为空。工具把带标记的条目列进报告的"白名单"一节，而不是让它们静默消失 ——
审计结论要看得见理由。

【退出码】
  0  没有未标注的可疑点
  1  存在未标注的可疑点，或自检失败
  2  用法错误

用法：
    tools/ct_audit.py                 扫描默认范围（src/）
    tools/ct_audit.py --quiet         只输出结论与未标注条目
    tools/ct_audit.py --self-test     反证：在合成样本上验证四条规则确有区分能力
    tools/ct_audit.py <路径> ...      扫描指定文件或目录
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 默认扫描范围：只看本项目自己写的密码学路径。
# hardware/ 是 RTL 与 Python 参考模型，cli/ 与 tests/ 不持有长期密钥材料。
DEFAULT_TARGETS = ["src"]

# 标记：`常量时间：<理由>`，半角冒号也接受，理由不得为空
ANNOTATION_RE = re.compile(r"常量时间\s*[:：]\s*(\S.*?)\s*(?:\*/)?$")

# ---- 秘密的判定 -------------------------------------------------------------
# 命中即视为秘密数据。整词匹配，大小写敏感（本仓库的秘密变量一律小写）。
SECRET_PATTERNS = [
    r"sk",                     # 私钥
    r"dk",                     # ML-KEM 解封装密钥
    r"[a-z0-9_]*seed[a-z0-9_]*",
    r"[a-z0-9_]*pin[a-z0-9_]*",
    r"kek", r"bek", r"rmk", r"cek", r"fkey", r"pin_key",
    r"ss",                     # KEM 共享秘密
    r"[a-z0-9_]*secret[a-z0-9_]*",
    r"[a-z0-9_]*verifier",
    r"g_root", r"root",
    r"coef",                   # Shamir 多项式系数
    r"rnd",                    # 签名随机化输入
    r"key", r"[a-z0-9_]*_key", r"key_[a-z0-9_]*",
    r"tag", r"[a-z0-9_]*_tag", r"mac", r"[a-z0-9_]*_mac",
    r"want",                   # 本仓库里恒为"重算出来的期望 tag/摘要"
    r"priv[a-z0-9_]*",
]

# 命中即排除。名字里带秘密词但本身是公开量（长度、容量、算法标识、状态位…）。
NON_SECRET_PATTERNS = [
    r"[a-z0-9_]*_len", r"[a-z0-9_]*_cap", r"[a-z0-9_]*_type",
    r"[a-z0-9_]*_id", r"[a-z0-9_]*_bit", r"[a-z0-9_]*_off",
    r"[a-z0-9_]*_fails", r"[a-z0-9_]*_count", r"[a-z0-9_]*_ready",
    r"[a-z0-9_]*_flags", r"[a-z0-9_]*_storage", r"[a-z0-9_]*_policy",
    r"[a-z0-9_]*_active", r"[a-z0-9_]*_ok", r"in_use", r"owner",
    r"has_[a-z0-9_]*", r"is_[a-z0-9_]*", r"n_[a-z0-9_]*",
    r"keystore[a-z0-9_]*", r"key_usage[a-z0-9_]*", r"keypair[a-z0-9_]*",
    r"[a-z0-9_]*_path", r"[a-z0-9_]*_name",
]

SECRET_RE = re.compile(r"^(?:%s)$" % "|".join(SECRET_PATTERNS))
NON_SECRET_RE = re.compile(r"^(?:%s)$" % "|".join(NON_SECRET_PATTERNS))

# 非常量时间的比较函数。CRYPTO_memcmp 与 pqc_ct_equal 是常量时间的，不在此列。
LEAKY_CMP = ("memcmp", "strcmp", "strncmp", "bcmp")

IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

# 一条访问链：base、若干下标与成员选择。`g_secrets[i].in_use` 取到的值是
# in_use 而不是 g_secrets，所以分类要看链尾而不是链首。
CHAIN_RE = re.compile(
    r"(?P<base>[A-Za-z_]\w*)"
    r"(?P<rest>(?:\s*(?:\[[^\[\]]*\]|(?:->|\.)\s*[A-Za-z_]\w*))*)")
MEMBER_RE = re.compile(r"(?:->|\.)\s*([A-Za-z_]\w*)")
INNER_PAREN_RE = re.compile(r"\(([^()]*)\)")


def is_secret(name: str) -> bool:
    return bool(SECRET_RE.match(name)) and not NON_SECRET_RE.match(name)


def strip_calls(expr: str) -> str:
    """把 f(...) 整体换成中性记号，并抹掉纯分组/强制转换用的括号。

    秘密数据**传进**某个函数，与该函数的**返回值**被拿去做判据，是两件事：
    `if (pqc_wrap(kek, ...) != 0)` 的分支只取决于返回码。真正要盯的是秘密值
    本身直接出现在判据里的情况。

    由内向外逐层化简，这样 `f(a, (int)b)` 这种带强制转换的实参也能正确识别成
    函数调用，而不是因为括号嵌套匹配不上就整条漏过去。
    """
    while True:
        m = INNER_PAREN_RE.search(expr)
        if not m:
            return expr
        head = expr[:m.start()].rstrip()
        if head and (head[-1].isalnum() or head[-1] == "_"):
            # 前面紧跟标识符 → 函数调用，整体丢弃
            cut = len(head)
            while cut > 0 and (head[cut - 1].isalnum() or head[cut - 1] == "_"):
                cut -= 1
            expr = expr[:cut] + " 0 " + expr[m.end():]
        else:
            # 分组或强制转换 → 去掉括号，保留内容
            expr = expr[:m.start()] + " " + m.group(1) + " " + expr[m.end():]


def value_names(expr: str) -> list[str]:
    """表达式里每条访问链最终取到的那个名字。"""
    names: list[str] = []
    for m in CHAIN_RE.finditer(expr):
        rest = m.group("rest")
        members = MEMBER_RE.findall(rest)
        names.append(members[-1] if members else m.group("base"))
    return names


def secrets_in(expr: str) -> list[str]:
    """表达式里取到的秘密数据，去重并保持出现顺序。"""
    seen: list[str] = []
    for n in value_names(expr):
        if is_secret(n) and n not in seen:
            seen.append(n)
    return seen


# ---- 词法预处理 -------------------------------------------------------------
def blank_out(src: str) -> tuple[str, list[str]]:
    """把注释与字符串/字符字面量替换成等长空白，返回（净化后的源码，逐行注释文本）。

    保持字符数与行数不变，这样净化后的偏移量仍可直接换算成行号；注释文本另行
    保留，供白名单标记查找。
    """
    out = list(src)
    comments: list[str] = [""] * (src.count("\n") + 1)
    i, n, line = 0, len(src), 0

    def record(text: str, at: int) -> None:
        for k, piece in enumerate(text.split("\n")):
            idx = at + k
            if idx < len(comments):
                comments[idx] += piece

    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        two = src[i:i + 2]
        if two == "/*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            record(src[i:j], line)
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            line += src.count("\n", i, j)
            i = j
            continue
        if two == "//":
            j = src.find("\n", i)
            j = n if j < 0 else j
            record(src[i:j], line)
            for k in range(i, j):
                out[k] = " "
            i = j
            continue
        if c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            line += src.count("\n", i, j)
            i = j
            continue
        i += 1
    return "".join(out), comments


def match_paren(text: str, open_pos: int) -> int:
    """返回与 text[open_pos] 处 '(' 配对的 ')' 的下标；找不到返回 -1。"""
    depth = 0
    for i in range(open_pos, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def match_bracket(text: str, open_pos: int) -> int:
    depth = 0
    for i in range(open_pos, len(text)):
        if text[i] == "[":
            depth += 1
        elif text[i] == "]":
            depth -= 1
            if depth == 0:
                return i
        elif text[i] in ";{}":
            return -1
    return -1


# ---- 发现项 -----------------------------------------------------------------
class Finding:
    def __init__(self, path: Path, line: int, rule: str, detail: str, snippet: str):
        self.path = path
        self.line = line
        self.rule = rule
        self.detail = detail
        self.snippet = snippet
        self.reason: str | None = None      # 非 None 表示已标注


def annotation_for(lines: list[str], comments: list[str], idx: int) -> str | None:
    """取第 idx 行（0 基）的白名单理由：同一行的注释，或紧贴上方的注释块。"""
    m = ANNOTATION_RE.search(comments[idx]) if idx < len(comments) else None
    if m:
        return m.group(1)
    k = idx - 1
    while k >= 0:
        stripped = lines[k].strip()
        has_comment = bool(comments[k].strip())
        if not has_comment:
            # 允许注释块与被标注语句之间隔着空行以外的东西时立即停止
            if stripped == "":
                k -= 1
                continue
            return None
        m = ANNOTATION_RE.search(comments[k])
        if m:
            return m.group(1)
        k -= 1
    return None


# ---- 四条规则 ---------------------------------------------------------------
def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos)


def scan_comparisons(path: Path, code: str, lines: list[str]) -> list[Finding]:
    out = []
    for fn in LEAKY_CMP:
        for m in re.finditer(r"(?<![A-Za-z0-9_])%s\s*\(" % fn, code):
            end = match_paren(code, m.end() - 1)
            if end < 0:
                continue
            args = code[m.end():end]
            found = secrets_in(args)
            if not found:
                continue
            ln = line_of(code, m.start())
            out.append(Finding(path, ln, "R1 比较",
                               "%s() 作用于秘密数据 %s" % (fn, "、".join(found)),
                               lines[ln].strip()))
    return out


PREFIX = r"(?:[A-Za-z_]\w*\s*(?:->|\.)\s*)*"


def split_atoms(cond: str) -> list[str]:
    """按顶层的 && / || 把判据拆成原子条件。"""
    atoms, depth, start = [], 0, 0
    i = 0
    while i < len(cond):
        c = cond[i]
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif depth == 0 and cond[i:i + 2] in ("&&", "||"):
            atoms.append(cond[start:i])
            i += 2
            start = i
            continue
        i += 1
    atoms.append(cond[start:])
    return atoms


def is_pointer_check(atom: str, name: str) -> bool:
    """该原子条件是否只是"指针是否为空"，而非对秘密数据取值。"""
    a = atom.strip()
    while a.startswith("(") and a.endswith(")"):
        a = a[1:-1].strip()
    a = a.lstrip("!").strip()
    n = re.escape(name)
    return bool(re.fullmatch(PREFIX + n, a) or
                re.fullmatch(PREFIX + n + r"\s*[!=]=\s*NULL", a) or
                re.fullmatch(r"NULL\s*[!=]=\s*" + PREFIX + n, a))


def secret_conditions(cond: str) -> list[str]:
    """判据里真正被取值的秘密数据。"""
    found: list[str] = []
    for atom in split_atoms(strip_calls(cond)):
        for s in secrets_in(atom):
            if s not in found and not is_pointer_check(atom, s):
                found.append(s)
    return found


def ternary_cond(code: str, qpos: int) -> str:
    """从 '?' 向左取出它的判据子表达式。

    在未闭合的 '(' 、逗号、赋值号或语句边界处停下 —— 否则 `f(a, sk, n ? x : y)`
    会把整条语句都当成判据，把无辜的实参算成分支依赖。
    """
    depth = 0
    i = qpos - 1
    while i >= 0:
        c = code[i]
        if c in ")]":
            depth += 1
        elif c in "([":
            if depth == 0:
                break
            depth -= 1
        elif depth == 0 and (c in ",;{}:" or (c == "=" and code[i - 1:i + 1] not in
                                              ("==", "!=", "<=", ">="))):
            break
        i -= 1
    return code[i + 1:qpos]


def scan_branches(path: Path, code: str, lines: list[str]) -> list[Finding]:
    out = []
    for m in re.finditer(r"(?<![A-Za-z0-9_])(if|while|switch)\s*\(", code):
        kw = m.group(1)
        end = match_paren(code, m.end() - 1)
        if end < 0:
            continue
        found = secret_conditions(code[m.end():end])
        if not found:
            continue
        ln = line_of(code, m.start())
        out.append(Finding(path, ln, "R2 分支",
                           "%s 的判据依赖秘密数据 %s" % (kw, "、".join(found)),
                           lines[ln].strip()))
    # 三元运算符：判据是 '?' 左边那一个子表达式，不是整条语句
    for m in re.finditer(r"\?", code):
        found = secret_conditions(ternary_cond(code, m.start()))
        if not found:
            continue
        ln = line_of(code, m.start())
        out.append(Finding(path, ln, "R2 分支",
                           "?: 的判据依赖秘密数据 %s" % "、".join(found),
                           lines[ln].strip()))
    return out


def scan_indices(path: Path, code: str, lines: list[str]) -> list[Finding]:
    out = []
    for m in re.finditer(r"\[", code):
        end = match_bracket(code, m.start())
        if end < 0:
            continue
        idx = code[m.start() + 1:end]
        found = secrets_in(idx)
        if not found:
            continue
        ln = line_of(code, m.start())
        out.append(Finding(path, ln, "R3 下标",
                           "数组下标由秘密数据 %s 派生" % "、".join(found),
                           lines[ln].strip()))
    return out


def scan_divisions(path: Path, code: str, lines: list[str]) -> list[Finding]:
    out = []
    for m in re.finditer(r"(?<![/*])([/%])(?![/*=])", code):
        op = m.group(1)
        # 除数：本行内运算符右侧到下一个定界符为止
        tail = code[m.end():]
        cut = min([p for p in (tail.find(";"), tail.find(","), tail.find(")"),
                               tail.find("\n")) if p >= 0] or [len(tail)])
        found = secrets_in(tail[:cut])
        if not found:
            continue
        ln = line_of(code, m.start())
        out.append(Finding(path, ln, "R4 除法",
                           "'%s' 的除数由秘密数据 %s 派生" % (op, "、".join(found)),
                           lines[ln].strip()))
    return out


def scan_file(path: Path) -> list[Finding]:
    src = path.read_text(encoding="utf-8")
    code, comments = blank_out(src)
    lines = src.split("\n")
    findings: list[Finding] = []
    findings += scan_comparisons(path, code, lines)
    findings += scan_branches(path, code, lines)
    findings += scan_indices(path, code, lines)
    findings += scan_divisions(path, code, lines)
    for f in findings:
        f.reason = annotation_for(lines, comments, f.line)
    findings.sort(key=lambda f: (str(f.path), f.line, f.rule))
    return findings


def collect(targets: list[Path]) -> list[Path]:
    files: list[Path] = []
    for t in targets:
        if t.is_dir():
            files += sorted(p for p in t.rglob("*") if p.suffix in (".c", ".h"))
        elif t.exists():
            files.append(t)
    return files


# ---- 报告 -------------------------------------------------------------------
def report(findings: list[Finding], scanned: int, quiet: bool) -> int:
    flagged = [f for f in findings if f.reason is None]
    allowed = [f for f in findings if f.reason is not None]

    print("常量时间审计：扫描 %d 个源文件，规则 R1 比较 / R2 分支 / R3 下标 / R4 除法"
          % scanned)
    print()

    if not quiet and allowed:
        print("白名单（%d 条，已审阅并给出理由）" % len(allowed))
        for f in allowed:
            rel = f.path.relative_to(ROOT) if f.path.is_absolute() else f.path
            print("  %s:%d  [%s] %s" % (rel, f.line + 1, f.rule, f.detail))
            print("      理由：%s" % f.reason)
        print()

    if flagged:
        print("未标注的可疑点（%d 条）" % len(flagged))
        for f in flagged:
            rel = f.path.relative_to(ROOT) if f.path.is_absolute() else f.path
            print("  ✗ %s:%d  [%s] %s" % (rel, f.line + 1, f.rule, f.detail))
            print("      %s" % f.snippet)
        print()
        print("处理方式：改成常量时间写法，或在该行/其上方注释块里写")
        print("          /* 常量时间：<为什么这里安全> */")
        return 1

    print("✓ 未发现未标注的可疑点（白名单 %d 条）" % len(allowed))
    return 0


# ---- 反证 -------------------------------------------------------------------
# 每条规则一个正样本（必须被抓到）和一个白名单样本（必须被放过）。
# 扫不出来就说明规则失效了 —— 这是"审计结论有区分能力"的依据。
SELF_TEST_BAD = """
#include <string.h>
static int r1(const unsigned char *tag, const unsigned char *want, unsigned n)
{
	return memcmp(tag, want, n) == 0;
}
static int r2(const unsigned char *sk)
{
	if (sk[0] == 0x42) {
		return 1;
	}
	return 0;
}
static unsigned char r3(const unsigned char *table, unsigned char secret)
{
	return table[secret];
}
static unsigned r4(unsigned x, unsigned pin)
{
	return x % pin;
}
"""

SELF_TEST_ANNOTATED = """
#include <string.h>
static int r1(const unsigned char *tag, const unsigned char *want, unsigned n)
{
	/* 常量时间：比较的是公开的文件头魔数，不是 tag 本身 */
	return memcmp(tag, want, n) == 0;
}
static int r2(const unsigned char *sk)
{
	/* 常量时间：判据只与公开的算法参数集有关 */
	if (sk[0] == 0x42) {
		return 1;
	}
	return 0;
}
static unsigned char r3(const unsigned char *table, unsigned char secret)
{
	return table[secret];   /* 常量时间：下标来自公开的分片索引 */
}
static unsigned r4(unsigned x, unsigned pin)
{
	/* 常量时间：除数是编译期常量，与 PIN 无关 */
	return x % pin;
}
"""

SELF_TEST_CLEAN = """
#include <string.h>
static int ok(const unsigned char *sk, const unsigned char *want, unsigned n)
{
	unsigned char diff = 0;
	unsigned i;
	if (!sk || !want) {
		return 0;
	}
	for (i = 0; i < n; i++) {
		diff |= (unsigned char)(sk[i] ^ want[i]);
	}
	return diff == 0;
}
"""


def self_test() -> int:
    import tempfile

    ok = True
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)

        bad = tmp / "bad.c"
        bad.write_text(SELF_TEST_BAD, encoding="utf-8")
        got = {f.rule.split()[0] for f in scan_file(bad) if f.reason is None}
        for rule in ("R1", "R2", "R3", "R4"):
            hit = rule in got
            print("  %s 正样本被抓到：%s" % (rule, "是" if hit else "否 ← 规则失效"))
            ok &= hit

        ann = tmp / "annotated.c"
        ann.write_text(SELF_TEST_ANNOTATED, encoding="utf-8")
        left = [f for f in scan_file(ann) if f.reason is None]
        print("  标注后剩余未标注条目：%d（应为 0）" % len(left))
        for f in left:
            print("      未被白名单覆盖：%s 行 %d" % (f.rule, f.line + 1))
        ok &= not left

        clean = tmp / "clean.c"
        clean.write_text(SELF_TEST_CLEAN, encoding="utf-8")
        noise = [f for f in scan_file(clean) if f.reason is None]
        print("  常量时间写法上的误报：%d（应为 0）" % len(noise))
        for f in noise:
            print("      误报：%s 行 %d %s" % (f.rule, f.line + 1, f.snippet))
        ok &= not noise

    print()
    if ok:
        print("✓ 四条规则均有区分能力，白名单标记生效，常量时间写法不误报")
        return 0
    print("✗ 自检失败：扫描规则不具备区分能力，审计结论不可信")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description="常量时间源码审计")
    ap.add_argument("targets", nargs="*", help="要扫描的文件或目录（默认 src/）")
    ap.add_argument("--quiet", action="store_true", help="不列出白名单条目")
    ap.add_argument("--self-test", action="store_true",
                    help="反证：在合成样本上验证规则确有区分能力")
    args = ap.parse_args()

    if args.self_test:
        print("常量时间审计规则自检")
        return self_test()

    targets = [Path(t) for t in args.targets] or [ROOT / t for t in DEFAULT_TARGETS]
    for t in targets:
        if not t.exists():
            print("找不到 %s" % t, file=sys.stderr)
            return 2
    files = collect(targets)
    if not files:
        print("没有可扫描的源文件", file=sys.stderr)
        return 2

    findings: list[Finding] = []
    for f in files:
        findings += scan_file(f)
    return report(findings, len(files), args.quiet)


if __name__ == "__main__":
    raise SystemExit(main())
