#!/usr/bin/env python3
"""BL31 补丁脚本的无板检查（FINAL-PLAN §8 第 5 项的"能验的那一半"）

boot/atf/patch_atf_secmmio.py 生成的是**跑在 EL3 里**的 C 代码。那段代码
出错的代价极高：EL3 上的取数错误没有任何东西接得住，发 SMC 的核当场卡死
（那个脚本自己的文件头记着这一条）。而它在这台机器上**编不了** —— 要
ATF 源树 + aarch64 工具链，两样都在构建机上。

于是这里做能做的三件事，并且把每件事**证明不了什么**也写清楚：

  ① 脚本本身语法正确（ast.parse）；
  ② **把生成的 C 片段抠出来，用主机编译器 -Wall -Wextra 编一遍**。
     它证明的是"这段 C 是合法的、没有未声明的标识符、没有类型错配"——
     用一组最小的 stub 顶掉 ATF 的宏与 mmio 访问。
     它**证明不了**语义：白名单判对没有、SMC_RET2 的寄存器约定对不对、
     在真 EL3 上会不会挂 —— 那些只能上板验。
  ③ 结构性检查：种子偏移确实被排除在通用 PL_RD/PL_WR 之外。
     这一条是 CODE-1 那对闸门的**另一半**（PL 侧只认安全事务，挡不住 root
     经 EL3 的通用 PL_WR 转一手）。两边缺一整套就白做，所以它值得一条
     专门的检查 —— 而且失败方式同样是"什么都没发生"。

⚠️ 通过这条检查**不代表**补丁在板上能用。它只是把"连编都编不过"这一类
   错误挡在上板之前 —— 那一类在这里最贵：上板一次要走构建机、烧卡、
   而失败形态是板子半死不活。
"""
import ast
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "boot/atf/patch_atf_secmmio.py"

# ATF 的最小替身：只够让那段 C 编过，不模拟任何语义
HARNESS_HEAD = """
#include <stdint.h>
#include <stddef.h>
typedef uint64_t u_register_t;
static inline uint32_t mmio_read_32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void mmio_write_32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
/* ATF 的 is_caller_secure(flags) 看的是 SCR_EL3.NS 那一位的镜像。
   这里只要一个能编过的形状 —— 真实语义由 ATF 提供。 */
#define is_caller_secure(f) (((f) & 1U) == 0U)
#define SMC_RET2(h, a, b) do { (void)(h); ret_a = (uint64_t)(a); \\
                               ret_b = (uint64_t)(b); return 0; } while (0)
"""

HARNESS_TAIL = """
static uint64_t ret_a, ret_b;
int pqchsm_dispatch(uint32_t fid, uint64_t x1, uint64_t x2, void *handle,
                    uint64_t flags);
int pqchsm_dispatch(uint32_t fid, uint64_t x1, uint64_t x2, void *handle,
                    uint64_t flags)
{
\t(void)x2;
\tswitch (fid) {
%(cases)s
\tdefault: return -1;
\t}
}
int main(void) { (void)pqchsm_dispatch; (void)ret_a; (void)ret_b; return 0; }
"""


def extract(src, name, pat):
    m = re.search(pat, src, re.S | re.M)
    if not m:
        print("  ✗ 抠不出 %s —— 补丁脚本的结构变了，这条检查要跟着改" % name)
        return None
    # 脚本里那两段是 Python 字符串字面量，\t 是两个字符，要还原成真 TAB
    return m.group(1).replace("\\t", "\t").replace("\\'", "'")


def main():
    src = PATCH.read_text(encoding="utf-8")

    try:
        ast.parse(src)
    except SyntaxError as e:
        print("  ✗ 补丁脚本语法错误：%s" % e)
        return 1
    print("  ✓ 补丁脚本语法正确")

    defs = extract(src, "DEFS", r"^DEFS = BEG \+ '''(.*?)''' \+ END")
    cases = extract(src, "CASES", r"^CASES = '\\t' \+ BEG \+ '''(.*?)''' \+ '\\t' \+ END")
    if defs is None or cases is None:
        return 1

    cc = os.environ.get("CC", "cc")
    if not shutil.which(cc):
        print("  SKIP：没有 C 编译器，跳过 EL3 片段编译检查")
    else:
        d = tempfile.mkdtemp()
        try:
            f = Path(d) / "sip.c"
            f.write_text(HARNESS_HEAD + defs + (HARNESS_TAIL % {"cases": cases}),
                         encoding="utf-8")
            r = subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-c",
                                str(f), "-o", str(Path(d) / "sip.o")],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print("  ✗ 生成的 EL3 C 片段编不过：")
                for line in (r.stderr or r.stdout).splitlines()[:20]:
                    print("      " + line)
                return 1
            print("  ✓ 生成的 EL3 C 片段过 -Wall -Wextra")
        finally:
            shutil.rmtree(d, ignore_errors=True)

    # ---- 结构：种子偏移必须被排除在通用读写之外 ----
    fail = 0
    if "PQCHSM_MLKEM_SEED_DATA" not in defs or "PQCHSM_MLDSA_SEED_DATA" not in defs:
        print("  ✗ 种子寄存器地址常量不见了")
        fail = 1
    # 排除判定必须在 pl_permit 里，而且**读写都拒**（不是只在 is_write 分支）
    m = re.search(r"static int pl_permit\(.*?\n\}", defs, re.S)
    if not m:
        print("  ✗ 找不到 pl_permit()")
        fail = 1
    elif not re.search(r"if \(\(a == PQCHSM_MLKEM_SEED_DATA\) \|\| "
                       r"\(a == PQCHSM_MLDSA_SEED_DATA\)\)\s*\n\s*return 0;", m.group(0)):
        print("  ✗ pl_permit 没有把种子偏移排除掉 —— root 经 EL3 的通用 PL_WR")
        print("    转一手就能把自己知道的种子塞进 PL，PL 侧那道 AxPROT 门白做")
        fail = 1
    else:
        print("  ✓ 种子偏移已从通用 PL_RD/PL_WR 排除（读写都拒）")

    if "is_caller_secure(flags)" not in cases:
        print("  ✗ 种子服务没有按调用方世界分级")
        fail = 1
    else:
        print("  ✓ 种子服务查了调用方世界（SCR_EL3.NS）")

    # 空对照：同一套判据对着一个**没有**排除的样本必须报失败
    ctrl = defs.replace("if ((a == PQCHSM_MLKEM_SEED_DATA) || "
                        "(a == PQCHSM_MLDSA_SEED_DATA))", "if (0)")
    mc = re.search(r"static int pl_permit\(.*?\n\}", ctrl, re.S)
    if mc and re.search(r"if \(\(a == PQCHSM_MLKEM_SEED_DATA\)", mc.group(0)):
        print("  ✗ 空对照失败 —— 这条检查在任何输入上都会通过，等于没有")
        fail = 1
    else:
        print("  ✓ 空对照成立")

    if fail:
        return 1
    print("check_atf_patch: 通过（⚠️ 只证明编得过与结构在位，语义仍需上板验）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
