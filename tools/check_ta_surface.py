#!/usr/bin/env python3
"""结构性回归：TA 那三条谕言机命令必须一直是删掉的（PS-22 / PS-24）

【为什么需要一条专门的检查】
删一段代码是一次性的事，**让它保持被删**不是。这三条命令有一个共同特点：
它们看起来都很有用（"给我派生一个子密钥"、"帮我解个包"），所以将来任何一个
需要"临时调试一下"的人都可能把它们加回来，而加回来之后：

  · 不会有任何用例失败 —— 产品路径本来就不用它们；
  · 不会有任何编译错误；
  · 唯一的症状是：任何普通世界进程又能把存储 KEK 要出来了。

这正是需要结构性检查的形状：**缺陷的表现是"什么都没发生"**。

检查四件事（都带空对照，见 --self-test）：
  ① TA 的分派 switch 里没有这三条 case；
  ② TA 里没有 cmd_kdf_derive / cmd_wrap / cmd_unwrap 的实现；
  ③ 普通世界客户端里没有对应的 shim；
  ④ 会话不再用 TEEC_LOGIN_PUBLIC，且 TA 侧确实拒绝 TEE_LOGIN_PUBLIC。

⚠️ 它证明不了的：命令**真的**没法通过别的路径达成同样效果。那需要的是
   对 TA 全部对外命令做一遍数据流分析，不是几条正则。这条检查的职责只有
   一个 —— 挡住"有人把它加回来"。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

TA_C     = ROOT / "tee/ta/pqchsm_ta.c"
CLIENT_C = ROOT / "tee/host/pqchsm_ta_client.c"
CLIENT_H = ROOT / "tee/host/pqchsm_ta_client.h"

# 被判定为"复活"的形状。注意都锚在**定义/调用**上，不是名字出现过就算 ——
# 墓碑注释里必然会提到这些名字，那正是我们希望留下的东西。
RULES = [
    (TA_C, r"case\s+TA_PQCHSM_CMD_KDF_DERIVE\s*:",  "TA 分派里又出现了 CMD_KDF_DERIVE"),
    (TA_C, r"case\s+TA_PQCHSM_CMD_WRAP\s*:",        "TA 分派里又出现了 CMD_WRAP"),
    (TA_C, r"case\s+TA_PQCHSM_CMD_UNWRAP\s*:",      "TA 分派里又出现了 CMD_UNWRAP"),
    (TA_C, r"^static\s+TEE_Result\s+cmd_kdf_derive\s*\(", "cmd_kdf_derive 又被实现了"),
    (TA_C, r"^static\s+TEE_Result\s+cmd_wrap\s*\(",       "cmd_wrap 又被实现了"),
    (TA_C, r"^static\s+TEE_Result\s+cmd_unwrap\s*\(",     "cmd_unwrap 又被实现了"),
    (CLIENT_C, r"^int\s+pqchsm_ta_kdf\s*\(",    "客户端 shim pqchsm_ta_kdf 又回来了"),
    (CLIENT_C, r"^int\s+pqchsm_ta_wrap\s*\(",   "客户端 shim pqchsm_ta_wrap 又回来了"),
    (CLIENT_C, r"^int\s+pqchsm_ta_unwrap\s*\(", "客户端 shim pqchsm_ta_unwrap 又回来了"),
    (CLIENT_H, r"^int\s+pqchsm_ta_kdf\s*\(",    "客户端头里又声明了 pqchsm_ta_kdf"),
    (CLIENT_H, r"^int\s+pqchsm_ta_wrap\s*\(",   "客户端头里又声明了 pqchsm_ta_wrap"),
    (CLIENT_H, r"^int\s+pqchsm_ta_unwrap\s*\(", "客户端头里又声明了 pqchsm_ta_unwrap"),
    # 登录类型：客户端不许再用 PUBLIC
    (CLIENT_C, r"TEEC_LOGIN_PUBLIC", "客户端又用回了 TEEC_LOGIN_PUBLIC"),
]

# 必须**存在**的东西（删掉了同样是回归）
MUST_HAVE = [
    (CLIENT_C, r"TEEC_LOGIN_USER", "客户端没有用 TEEC_LOGIN_USER 打开会话"),
    (TA_C, r"TEE_GetPropertyAsIdentity", "TA 不再查调用方身份"),
    (TA_C, r"id\.login\s*==\s*TEE_LOGIN_PUBLIC", "TA 不再拒绝匿名（PUBLIC）会话"),
]


# 墓碑注释里必然会写出这些名字（那正是要留下的东西），所以扫描前先把注释
# 剥掉，只看**代码**。不剥的话这条检查会被自己的说明文字触发 —— 实测踩过。
_BLOCK = re.compile(r"/\*.*?\*/", re.S)
_LINE  = re.compile(r"//[^\n]*")


def strip_comments(text: str) -> str:
    """去掉 C 注释，但保留行数（换行照原样留着，报错行号才不会错位）"""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    return _LINE.sub(blank, _BLOCK.sub(blank, text))


def self_test():
    """先验扫描器：合成样本上必须能抓、且不误报。

    顺序不能反 —— 一个抓不出东西的扫描器，扫出"没问题"毫无意义。
    """
    bad = """
    TEE_Result f(void) {
        switch (c) {
        case TA_PQCHSM_CMD_UNWRAP:
            return cmd_unwrap(t, p);
        }
    }
    """
    good = """
    /* 墓碑：CMD_UNWRAP 已删除，编号保留不复用。别加回来。 */
    TEE_Result f(void) {
        switch (c) {
        case TA_PQCHSM_CMD_KEK_SET:
            return cmd_kek_set(t, p);
        }
    }
    """
    # 第三个样本：**注释里**原样写着被禁的名字。这是最容易误报的一种，
    # 而且它一定会出现 —— 墓碑注释本身就得把名字写出来。
    tomb = """
    /* CMD_UNWRAP 已删除。别写 case TA_PQCHSM_CMD_UNWRAP: 加回来。
       客户端也不许再用 TEEC_LOGIN_PUBLIC。 */
    TEE_Result f(void) { return TEE_SUCCESS; }
    """
    pat = r"case\s+TA_PQCHSM_CMD_UNWRAP\s*:"
    ok = True
    if not re.search(pat, strip_comments(bad), re.MULTILINE):
        print("  ✗ 自测：抓不到复活的 case"); ok = False
    if re.search(pat, strip_comments(good), re.MULTILINE):
        print("  ✗ 自测：把正常代码误判成复活"); ok = False
    if re.search(pat, strip_comments(tomb), re.MULTILINE):
        print("  ✗ 自测：把墓碑注释里的名字误判成复活"); ok = False
    if re.search(r"TEEC_LOGIN_PUBLIC", strip_comments(tomb)):
        print("  ✗ 自测：注释里的 TEEC_LOGIN_PUBLIC 被当成了真的用法"); ok = False
    if ok:
        print("  ✓ 自测通过（能抓、不误报）")
    return ok


def main():
    if "--self-test" in sys.argv:
        return 0 if self_test() else 1

    if not self_test():
        return 1

    fail = 0
    for path, pat, why in RULES:
        if not path.exists():
            print(f"  ✗ 找不到 {path.relative_to(ROOT)}")
            fail = 1
            continue
        if re.search(pat, strip_comments(path.read_text(encoding="utf-8")),
                     re.MULTILINE):
            print(f"  ✗ {path.relative_to(ROOT)}：{why}")
            fail = 1

    for path, pat, why in MUST_HAVE:
        if not re.search(pat, strip_comments(path.read_text(encoding="utf-8")),
                         re.MULTILINE):
            print(f"  ✗ {path.relative_to(ROOT)}：{why}")
            fail = 1

    if fail:
        print("\nPS-22 / PS-24 那两颗地雷回来了。理由见 "
              "tee/include/pqchsm_ta_proto.h 的墓碑注释。")
        return 1
    print(f"  ✓ TA 对外面：三条谕言机命令仍是删掉的，会话不接受匿名登录")
    print("check_ta_surface: 通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
