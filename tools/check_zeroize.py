#!/usr/bin/env python3
"""结构性回归：持有密钥材料的结构体必须在销毁函数里被清零，函数里的局部密钥
缓冲不得在未清零的情况下返回。

这与 tools/check_no_readback.py 是同一类检查：清零纪律靠人记不住，靠单元测试
也只能覆盖被测到的那几条路径 —— 少写一个 `pqc_secure_zero`、或在中途加一条
提前 return，功能测试全绿而密钥留在了内存里。把纪律写成扫描规则，每次回归都
重扫一遍，遗漏才会当场暴露。

【两条检查】
  A 结构体覆盖  含密钥字段的结构体，其销毁函数必须清掉每一个密钥字段
  B 返回路径    函数里的局部密钥缓冲，在写入之后的每条 return 上都必须已清零

检查 B 是线性近似：它按文本顺序推进，不做控制流分析，因此对"写入分支与 return
分支互斥"的写法会误报。误报用注释标记声明：

    /* 无需清零：此处 kek 尚未写入，派生失败时缓冲仍是未初始化状态 */
    return -1;

标记写在缓冲的声明处（整条豁免）、结构体字段的声明处，或某一条具体的 return 上；
理由跟在冒号之后、写在同一行内，不能为空。带标记的条目会列进报告的"豁免"一节，
而不是静默消失。

【退出码】
  0  没有未标注的遗漏
  1  存在未标注的遗漏，或自检失败
  2  用法错误

用法：
    tools/check_zeroize.py                扫描默认范围（src/ 与 include/）
    tools/check_zeroize.py --self-test    反证：验证两条检查确有区分能力
    tools/check_zeroize.py <路径> ...     扫描指定文件或目录
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

# 秘密命名的判定与词法预处理与常量时间审计共用一套，两个工具对"什么算密钥
# 材料"必须给出同一个答案，否则两份报告没法互相印证。
from ct_audit import blank_out, is_secret, match_paren  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
# ⚠️ service/ 里**进了产物的那两个文件**必须在扫描范围里：libsdfe.c 与
# sdfe_pkenc.c 都被编译进 libpqchsm（见 CMakeLists）。漏扫的直接后果已经
# 出现过 —— sdfe_pkenc.c 有四条早退路径带着 32 字节共享密钥直接 return，
# 清零用的还是会被 -O2 消掉的裸 memset。**凡是进了产物的就要在这张表里。**
#
# ⚠️ **两个独立二进制暂未纳入，这是一处已知缺口，不是漏了：**
#   · service/pqchsm_fpgad.c —— 板上 daemon，89 处待判（多为请求载荷缓冲
#     pay/out，它们确实过密钥材料，但要一条条判"这条路径上有没有秘密"，
#     属于单独一轮）；
#   · service/sdf_demo.c —— 示例程序，只链接 libsdfe，拿不到 pqc_secure_zero。
# 纳入它们之前，**不要**声称 service 层已被清零审计全覆盖。
DEFAULT_TARGETS = ["src", "include",
                   "service/libsdfe.c", "service/sdfe_pkenc.c"]

ANNOTATION_RE = re.compile(r"无需清零\s*[:：]\s*(\S.*?)\s*(?:\*/)?$")

# 销毁语义的函数名
DTOR_RE = re.compile(r"(?:free|destroy|wipe|zeroize|clear|drop|release|fini)")

# 必须出现在扫描结果里，否则说明扫错了范围
EXPECTED_STRUCTS = ["slot_t", "p11_secret_t"]

ZERO_CALL = r"pqc_secure_(?:zero|free)"

# 覆盖集合里的哨兵：表示"整个结构体被一次性清零"
WHOLE = "\x00whole"

# 只有这些类型的局部数组才算密钥缓冲；结构体与指针另由检查 A 覆盖
BUF_TYPES = r"(?:uint8_t|unsigned\s+char|int16_t|uint32_t|uint64_t|char)"


# ---- 通用 -------------------------------------------------------------------
def annotation_for(lines: list[str], comments: list[str], idx: int) -> str | None:
    """取第 idx 行（0 基）的豁免理由：同一行的注释，或紧贴上方的注释块。"""
    if idx < len(comments):
        m = ANNOTATION_RE.search(comments[idx])
        if m:
            return m.group(1)
    k = idx - 1
    while k >= 0:
        if not comments[k].strip():
            if lines[k].strip() == "":
                k -= 1
                continue
            return None
        m = ANNOTATION_RE.search(comments[k])
        if m:
            return m.group(1)
        k -= 1
    return None


def brace_end(code: str, open_pos: int) -> int:
    depth = 0
    for i in range(open_pos, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return len(code) - 1


def declarator_names(decl: str) -> list[str]:
    """从一条声明里取出被声明的名字（去掉类型、指针与数组维度）。"""
    names = []
    for part in decl.split(","):
        m = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*$", part.strip())
        if m:
            names.append(m.group(1))
    return names


# ---- 结构体与函数的提取 -----------------------------------------------------
class Struct:
    def __init__(self, name: str, path: Path, line: int):
        self.name = name
        self.path = path
        self.line = line
        self.fields: list[tuple[str, int]] = []      # (字段名, 行号)
        self.aliases: set[str] = {name}


TYPEDEF_RE = re.compile(r"typedef\s+struct(?:\s+(\w+))?\s*\{", re.S)
TAGGED_RE = re.compile(r"(?<!typedef\s)\bstruct\s+(\w+)\s*\{")
FUNC_RE = re.compile(r"^([A-Za-z_][\w \t\*]*?)\b(\w+)\s*\(([^;{}]*)\)\s*\n?\{", re.M)


def parse_structs(path: Path, code: str) -> list[Struct]:
    out: list[Struct] = []
    seen_spans: list[tuple[int, int]] = []

    def body_and_name(open_brace: int) -> tuple[str, int]:
        end = brace_end(code, open_brace)
        return code[open_brace + 1:end], end

    for m in TYPEDEF_RE.finditer(code):
        open_brace = code.index("{", m.start())
        body, end = body_and_name(open_brace)
        tail = re.match(r"\s*(\w+)\s*;", code[end + 1:])
        name = tail.group(1) if tail else (m.group(1) or "")
        if not name:
            continue
        s = Struct(name, path, code.count("\n", 0, m.start()))
        if m.group(1):
            s.aliases.add(m.group(1))
        fill_fields(s, body, code.count("\n", 0, open_brace))
        out.append(s)
        seen_spans.append((m.start(), end))

    for m in TAGGED_RE.finditer(code):
        if any(a <= m.start() <= b for a, b in seen_spans):
            continue
        open_brace = code.index("{", m.start())
        body, _ = body_and_name(open_brace)
        if not body.strip():
            continue
        s = Struct(m.group(1), path, code.count("\n", 0, m.start()))
        fill_fields(s, body, code.count("\n", 0, open_brace))
        out.append(s)
    return out


def fill_fields(s: Struct, body: str, body_start_line: int) -> None:
    off = 0
    for decl in body.split(";"):
        line = body_start_line + body.count("\n", 0, off + len(decl))
        off += len(decl) + 1
        # 函数指针成员不是数据字段，它的形参名会被当成字段名读进来
        if "(" in decl:
            continue
        for n in declarator_names(decl):
            s.fields.append((n, line))


class Func:
    def __init__(self, name: str, params: str, body: str, path: Path, line: int,
                 body_line: int):
        self.name = name
        self.params = params
        self.body = body
        self.path = path
        self.line = line
        self.body_line = body_line      # 函数体第一行（'{' 所在行）的 0 基行号


def parse_funcs(path: Path, code: str) -> list[Func]:
    out = []
    for m in FUNC_RE.finditer(code):
        if m.group(2) in ("if", "while", "for", "switch", "return", "sizeof"):
            continue
        open_brace = code.index("{", m.end() - 1)
        end = brace_end(code, open_brace)
        out.append(Func(m.group(2), m.group(3), code[open_brace:end + 1], path,
                        code.count("\n", 0, m.start()),
                        code.count("\n", 0, open_brace)))
    return out


# ---- 检查 A：结构体销毁覆盖 -------------------------------------------------
class Problem:
    def __init__(self, path: Path, line: int, kind: str, detail: str, snippet: str):
        self.path = path
        self.line = line
        self.kind = kind
        self.detail = detail
        self.snippet = snippet
        self.reason: str | None = None


def refers_to_type(f: Func, s: Struct, globals_of_type: set[str]) -> bool:
    """该函数是否作用在这个结构体上：形参是它，或者它操作了该类型的文件级变量。"""
    for alias in s.aliases:
        if re.search(r"\b(?:struct\s+)?%s\b" % re.escape(alias), f.params):
            return True
    return any(re.search(r"\b%s\b" % re.escape(g), f.body) for g in globals_of_type)


def split_args(text: str) -> list[str]:
    """按顶层逗号切分实参表。"""
    parts, depth, start = [], 0, 0
    for i, c in enumerate(text):
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    parts.append(text[start:])
    return [p.strip() for p in parts]


def zero_calls(body: str) -> list[list[str]]:
    """函数体里所有 pqc_secure_zero / pqc_secure_free 的实参表。"""
    out = []
    for m in re.finditer(ZERO_CALL + r"\s*\(", body):
        end = match_paren(body, m.end() - 1)
        if end >= 0:
            out.append(split_args(body[m.end():end]))
    return out


def norm(expr: str) -> str:
    return re.sub(r"\s+", "", expr).lstrip("&*")


def terminal_name(expr: str) -> str:
    members = re.findall(r"(?:->|\.)\s*([A-Za-z_]\w*)", expr)
    if members:
        return members[-1]
    m = re.match(r"\s*[&*]*\s*([A-Za-z_]\w*)", expr)
    return m.group(1) if m else ""


def base_name(expr: str) -> str:
    m = re.match(r"\s*[&*]*\s*([A-Za-z_]\w*)", expr)
    return m.group(1) if m else ""


def covered_fields(f: Func, s: Struct, all_funcs: dict[str, Func],
                   globals_of_type: set[str], seen: set[str]) -> set[str]:
    """f（及其调用到的、同样作用在 s 上的函数）清掉了 s 的哪些字段。"""
    if f.name in seen:
        return set()
    seen.add(f.name)
    got: set[str] = set()
    field_names = {n for n, _ in s.fields}
    param_names = set(declarator_names(f.params))

    for args in zero_calls(f.body):
        if not args:
            continue
        target = args[0]
        got.add(terminal_name(target))

        # 整体清零：pqc_secure_zero(x, sizeof(x)) / (x, sizeof(*x))。
        # 必须是结构体本身而不是它的某个字段，否则清一个字段就会被误算成清了全部。
        if (len(args) > 1 and args[1].startswith("sizeof")
                and terminal_name(target) not in field_names
                and base_name(target) in (param_names | globals_of_type)):
            inner = args[1][len("sizeof"):].strip()
            if norm(inner).strip("()") == norm(target):
                got |= field_names | {WHOLE}

    for callee in set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", f.body)):
        g = all_funcs.get(callee)
        if g and g is not f and refers_to_type(g, s, globals_of_type):
            got |= covered_fields(g, s, all_funcs, globals_of_type, seen)
    return got & (field_names | {WHOLE})


def check_structs(structs: list[Struct], funcs: list[Func], code_by_path: dict,
                  text_by_path: dict,
                  comments_by_path: dict) -> tuple[list[Problem], list[tuple[str, str]]]:
    by_name = {f.name: f for f in funcs}
    problems: list[Problem] = []
    summary: list[tuple[str, str]] = []

    for s in structs:
        lines = text_by_path[s.path].split("\n")
        comments = comments_by_path[s.path]
        # 字段上的豁免标记优先于命名判定：被声明为"不含密钥材料"的字段不参与检查
        secret_fields = {n for n, ln in s.fields
                         if is_secret(n) and annotation_for(lines, comments, ln) is None}
        exempt = {n: annotation_for(lines, comments, ln) for n, ln in s.fields
                  if is_secret(n) and annotation_for(lines, comments, ln) is not None}
        whole_struct = any(is_secret(a) for a in s.aliases)
        # 豁免条目一律进报告：审计要看得见每一条"为什么不清"的理由
        for n, why in exempt.items():
            ln = next(l for m, l in s.fields if m == n)
            p = Problem(s.path, ln, "A 结构体覆盖",
                        "%s.%s 声明为不含密钥材料" % (s.name, n), lines[ln].strip())
            p.reason = why
            problems.append(p)
        if not secret_fields and not whole_struct:
            continue

        code = code_by_path[s.path]
        globals_of_type = set()
        for alias in s.aliases:
            for m in re.finditer(
                    r"^(?:static\s+)?(?:struct\s+)?%s\s+([^;=\n(]+);" % re.escape(alias),
                    code, re.M):
                globals_of_type |= set(declarator_names(m.group(1)))

        dtors = [f for f in funcs
                 if DTOR_RE.search(f.name) and refers_to_type(f, s, globals_of_type)]
        if not dtors:
            problems.append(Problem(s.path, s.line, "A 结构体覆盖",
                                    "%s 持有密钥材料，但找不到销毁函数" % s.name,
                                    lines[s.line].strip()))
            continue

        covered: set[str] = set()
        for d in dtors:
            covered |= covered_fields(d, s, by_name, globals_of_type, set())

        required = set(secret_fields)
        if whole_struct:
            # 结构体本身就以"秘密"命名（会话密钥对象一类），要求整体清零，
            # 逐字段清会随着新增字段悄悄失效
            required.add(WHOLE)
        missing = sorted(required - covered)
        names = "、".join(sorted(d.name for d in dtors))
        if missing:
            for f in missing:
                if f == WHOLE:
                    problems.append(Problem(s.path, s.line, "A 结构体覆盖",
                                            "%s 未在 %s 中整体清零" % (s.name, names),
                                            lines[s.line].strip()))
                    continue
                ln = next(l for n, l in s.fields if n == f)
                problems.append(Problem(s.path, ln, "A 结构体覆盖",
                                        "%s.%s 未在 %s 中清零" % (s.name, f, names),
                                        lines[ln].strip()))
        else:
            what = ("整体清零" if whole_struct and not secret_fields
                    else "%d 个密钥字段" % len(secret_fields))
            summary.append((s.name, "%s ← %s" % (what, names)))
    return problems, summary


# ---- 检查 B：局部密钥缓冲的返回路径 -----------------------------------------
def check_locals(funcs: list[Func], text_by_path: dict,
                 comments_by_path: dict) -> tuple[list[Problem], list[Problem]]:
    problems: list[Problem] = []
    exempt: list[Problem] = []
    decl_re = re.compile(r"^\s*(?:static\s+)?(?:const\s+)?%s\s+([^;=]*\[[^;]*)\s*;\s*$"
                         % BUF_TYPES)

    for f in funcs:
        lines = f.body.split("\n")
        src_lines = text_by_path[f.path].split("\n")
        comments = comments_by_path[f.path]
        params = set(declarator_names(f.params))
        tracked: dict[str, int] = {}        # 缓冲名 → 声明所在行（函数体内偏移）
        live: set[str] = set()
        for i, line in enumerate(lines):
            m = decl_re.match(line)
            if m:
                # 声明处的豁免标记覆盖整个缓冲：声明它压根不是密钥材料
                why = annotation_for(src_lines, comments, f.body_line + i)
                for n in declarator_names(m.group(1)):
                    if not is_secret(n) or n in params:
                        continue
                    if why:
                        p = Problem(f.path, f.body_line + i, "B 返回路径",
                                    "%s() 的 %s 声明为不含密钥材料" % (f.name, n),
                                    line.strip())
                        p.reason = why
                        exempt.append(p)
                    else:
                        tracked[n] = i
                continue
            for n in list(tracked):
                if re.search(ZERO_CALL + r"\s*\(\s*&?\s*%s\b" % re.escape(n), line):
                    live.discard(n)
                elif re.search(r"\b%s\b" % re.escape(n), line):
                    live.add(n)
            if live and re.search(r"\breturn\b", line):
                ln = f.body_line + i
                for n in sorted(live):
                    problems.append(Problem(
                        f.path, ln, "B 返回路径",
                        "%s() 在 return 时未清零局部密钥缓冲 %s" % (f.name, n),
                        line.strip()))
    return problems, exempt


# ---- 驱动 -------------------------------------------------------------------
def analyse(files: list[Path]):
    structs: list[Struct] = []
    funcs: list[Func] = []
    code_by_path, text_by_path, comments_by_path = {}, {}, {}
    for p in files:
        text = p.read_text(encoding="utf-8")
        code, comments = blank_out(text)
        code_by_path[p], text_by_path[p], comments_by_path[p] = code, text, comments
        structs += parse_structs(p, code)
        funcs += parse_funcs(p, code)
    return structs, funcs, code_by_path, text_by_path, comments_by_path


def collect(targets: list[Path]) -> list[Path]:
    files: list[Path] = []
    for t in targets:
        if t.is_dir():
            files += sorted(p for p in t.rglob("*") if p.suffix in (".c", ".h"))
        elif t.exists():
            files.append(t)
    return files


def run(files: list[Path], require_expected: bool) -> int:
    structs, funcs, code_by_path, text_by_path, comments_by_path = analyse(files)
    problems, summary = check_structs(structs, funcs, code_by_path,
                                      text_by_path, comments_by_path)
    local_problems, local_exempt = check_locals(funcs, text_by_path,
                                                comments_by_path)
    problems += local_problems

    for p in problems:
        p.reason = annotation_for(text_by_path[p.path].split("\n"),
                                  comments_by_path[p.path], p.line)
    problems += local_exempt
    problems.sort(key=lambda p: (str(p.path), p.line))

    print("zeroize 结构性检查：%d 个源文件，%d 个结构体，%d 个函数"
          % (len(files), len(structs), len(funcs)))
    print()

    if require_expected:
        names = {s.name for s in structs}
        missing = [n for n in EXPECTED_STRUCTS if n not in names]
        if missing:
            print("✗ 没扫到 %s —— 扫描范围可能不对" % "、".join(missing),
                  file=sys.stderr)
            return 1

    if summary:
        print("已覆盖的密钥结构体")
        for name, detail in sorted(summary):
            print("  ✓ %-16s %s" % (name, detail))
        print()

    allowed = [p for p in problems if p.reason is not None]
    if allowed:
        print("豁免（%d 条，已审阅并给出理由）" % len(allowed))
        for p in allowed:
            rel = p.path.relative_to(ROOT) if p.path.is_absolute() else p.path
            print("  %s:%d  [%s] %s" % (rel, p.line + 1, p.kind, p.detail))
            print("      理由：%s" % p.reason)
        print()

    flagged = [p for p in problems if p.reason is None]
    if flagged:
        print("未标注的遗漏（%d 条）" % len(flagged))
        for p in flagged:
            rel = p.path.relative_to(ROOT) if p.path.is_absolute() else p.path
            print("  ✗ %s:%d  [%s] %s" % (rel, p.line + 1, p.kind, p.detail))
            print("      %s" % p.snippet)
        print()
        print("处理方式：补上 pqc_secure_zero，或在该行/其上方注释块里写")
        print("          /* 无需清零：<为什么这里没有残留> */")
        return 1

    print("✓ 密钥结构体的销毁路径与局部缓冲的返回路径均已覆盖（豁免 %d 条）"
          % len(allowed))
    return 0


# ---- 反证 -------------------------------------------------------------------
SELF_TEST_BAD = """
#include <string.h>
typedef struct {
	unsigned char pin_key[32];
	unsigned char user_verifier[32];
	int has_pin;
} probe_slot_t;

void probe_slot_wipe(probe_slot_t *s)
{
	pqc_secure_zero(s->pin_key, sizeof(s->pin_key));
	s->has_pin = 0;
}

int probe_derive(const unsigned char *in, unsigned n)
{
	unsigned char kek[32];
	if (derive(in, n, kek) != 0) {
		return -1;
	}
	pqc_secure_zero(kek, sizeof(kek));
	return 0;
}
"""

SELF_TEST_GOOD = """
#include <string.h>
typedef struct {
	unsigned char pin_key[32];
	unsigned char user_verifier[32];
	int has_pin;
} probe_slot_t;

void probe_slot_wipe(probe_slot_t *s)
{
	pqc_secure_zero(s->pin_key, sizeof(s->pin_key));
	pqc_secure_zero(s->user_verifier, sizeof(s->user_verifier));
	s->has_pin = 0;
}

int probe_derive(const unsigned char *in, unsigned n)
{
	unsigned char kek[32];
	if (derive(in, n, kek) != 0) {
		pqc_secure_zero(kek, sizeof(kek));
		return -1;
	}
	pqc_secure_zero(kek, sizeof(kek));
	return 0;
}
"""

SELF_TEST_ANNOTATED = """
#include <string.h>
typedef struct {
	unsigned char pin_key[32];
	/* 无需清零：该字段是公开的派生盐，不含密钥材料 */
	unsigned char user_verifier[32];
	int has_pin;
} probe_slot_t;

void probe_slot_wipe(probe_slot_t *s)
{
	pqc_secure_zero(s->pin_key, sizeof(s->pin_key));
	s->has_pin = 0;
}

int probe_derive(const unsigned char *in, unsigned n)
{
	unsigned char kek[32];
	if (derive(in, n, kek) != 0) {
		/* 无需清零：派生失败时 kek 仍是未初始化状态，没有秘密写进去 */
		return -1;
	}
	pqc_secure_zero(kek, sizeof(kek));
	return 0;
}
"""


def self_test() -> int:
    import tempfile

    ok = True
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)

        def scan(text: str) -> list[Problem]:
            p = tmp / "probe.c"
            p.write_text(text, encoding="utf-8")
            structs, funcs, code_by_path, text_by_path, comments_by_path = analyse([p])
            probs, _ = check_structs(structs, funcs, code_by_path,
                                     text_by_path, comments_by_path)
            loc, _ = check_locals(funcs, text_by_path, comments_by_path)
            probs += loc
            for x in probs:
                if x.reason is None:
                    x.reason = annotation_for(text_by_path[x.path].split("\n"),
                                              comments_by_path[x.path], x.line)
            return [x for x in probs if x.reason is None]

        bad = {p.kind.split()[0] for p in scan(SELF_TEST_BAD)}
        for kind, what in (("A", "漏清结构体字段"), ("B", "提前 return 未清零")):
            hit = kind in bad
            print("  检查 %s（%s）被抓到：%s" % (kind, what, "是" if hit else "否 ← 规则失效"))
            ok &= hit

        good = scan(SELF_TEST_GOOD)
        print("  补齐清零后的误报：%d（应为 0）" % len(good))
        for p in good:
            print("      误报：%s %s" % (p.kind, p.detail))
        ok &= not good

        ann = scan(SELF_TEST_ANNOTATED)
        print("  标注后剩余未标注条目：%d（应为 0）" % len(ann))
        for p in ann:
            print("      未被豁免覆盖：%s %s" % (p.kind, p.detail))
        ok &= not ann

    print()
    if ok:
        print("✓ 两条检查均有区分能力，豁免标记生效，清零到位的写法不误报")
        return 0
    print("✗ 自检失败：检查规则不具备区分能力，结论不可信")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description="zeroize 结构性检查")
    ap.add_argument("targets", nargs="*", help="要扫描的文件或目录（默认 src/ include/）")
    ap.add_argument("--self-test", action="store_true",
                    help="反证：验证两条检查确有区分能力")
    args = ap.parse_args()

    if args.self_test:
        print("zeroize 检查规则自检")
        return self_test()

    explicit = bool(args.targets)
    targets = [Path(t) for t in args.targets] or [ROOT / t for t in DEFAULT_TARGETS]
    for t in targets:
        if not t.exists():
            print("找不到 %s" % t, file=sys.stderr)
            return 2
    files = collect(targets)
    if not files:
        print("没有可扫描的源文件", file=sys.stderr)
        return 2
    return run(files, require_expected=not explicit)


if __name__ == "__main__":
    raise SystemExit(main())
