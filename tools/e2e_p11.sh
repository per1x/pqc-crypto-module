#!/usr/bin/env bash
# Level A：**从 PKCS#11 层驱动**的端到端链
#
#   [应用面 · PKCS#11]  初始化 token → 生成 ML-DSA 密钥 → 签名 → 导出公钥
#   [运维面 · admin  ]  备份（M-of-N 分片）→ 整机清零
#   [应用面 · PKCS#11]  确认密钥真的没了（签不出来）
#   [运维面 · admin  ]  用备份 + M 份分片恢复
#   [应用面 · PKCS#11]  再签一次 —— **必须能被清零之前那把公钥验过**
#
# 分工是有意的：PKCS#11 里没有备份/恢复/清零，它是应用面接口。
# 真实 HSM 也是这么分的（应用走 P11，运维走厂商工具），两边共用同一个
# 密钥库文件、**顺序访问**。这条脚本演示的正是这个交接。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODULE="${1:-$ROOT/build/pqchsm-pkcs11.dylib}"
ADMIN="$ROOT/build/pqchsm-admin"
PY="$ROOT/.venv-p11/bin/python"

[ -f "$MODULE" ] || { echo "SKIP: 没有 $MODULE"; exit 0; }
[ -x "$ADMIN" ]  || { echo "SKIP: 没有 $ADMIN（cmake --build build --target pqchsm-admin）"; exit 0; }
if ! { [ -x "$PY" ] && "$PY" -c "import PyKCS11" 2>/dev/null; }; then
  echo "SKIP: 没有 PyKCS11 环境"; exit 0
fi

W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
export PQCHSM_KEYSTORE="$W/ks.bin"
export PQCHSM_SLOTS=2
SO_PIN=12345678
USER_PIN=1234abcd
MSG="level-A over pkcs11"

pass=0; fail=0
ck() { if [ "$1" = ok ]; then echo "  ✓ $2"; pass=$((pass+1)); else echo "  ✗ $2"; fail=$((fail+1)); fi; }

# ---------- 应用面：P11 生成密钥并签名 ----------
echo "== [应用面 · PKCS#11] 初始化 token、生成 ML-DSA-65、签名 =="
"$PY" - "$MODULE" "$W" "$SO_PIN" "$USER_PIN" "$MSG" <<'PYEOF'
import sys, PyKCS11
mod, work, so_pin, user_pin, msg = sys.argv[1:6]
CKM_ML_DSA_KEYGEN, CKM_ML_DSA, CKA_PARAM = 0x1c, 0x1d, 0x61d
for n, v in (("CKM_ML_DSA_KEY_PAIR_GEN", CKM_ML_DSA_KEYGEN), ("CKM_ML_DSA", CKM_ML_DSA)):
    PyKCS11.CKM[n] = v; PyKCS11.CKM[v] = n
lib = PyKCS11.PyKCS11Lib(); lib.load(mod)
slot = lib.getSlotList(True)[0]
lib.initToken(slot, so_pin, "p11e2e")
s = lib.openSession(slot, PyKCS11.CKF_SERIAL_SESSION | PyKCS11.CKF_RW_SESSION)
s.login(so_pin, user_type=PyKCS11.CKU_SO); s.initPin(user_pin); s.logout()
s.login(user_pin)
pub, priv = s.generateKeyPair(
    [(CKA_PARAM, list((2).to_bytes(8, "little")))], [],
    mecha=PyKCS11.Mechanism(CKM_ML_DSA_KEYGEN, None))
pk = bytes(s.getAttributeValue(pub, [PyKCS11.CKA_VALUE])[0])
sig = bytes(s.sign(priv, msg.encode(), PyKCS11.Mechanism(CKM_ML_DSA, None)))
open(f"{work}/pk.bin", "wb").write(pk)
open(f"{work}/sig1.bin", "wb").write(sig)
print(f"    公钥 {len(pk)} B，签名 {len(sig)} B")
s.logout(); s.closeSession()
PYEOF
[ $? -eq 0 ] && ck ok "P11 生成密钥并签名" || ck err "P11 生成密钥并签名"
[ -s "$W/pk.bin" ] && ck ok "公钥已导出（$(wc -c <"$W/pk.bin" | tr -d ' ') B）" || ck err "公钥导出"

# ---------- 运维面：备份 ----------
echo
echo "== [运维面 · admin] 列出槽位 → 备份（3-of-5）=="
"$ADMIN" -k "$PQCHSM_KEYSTORE" -n 2 list | sed 's/^/    /'
if "$ADMIN" -k "$PQCHSM_KEYSTORE" -n 2 backup "$W/backup.bin" "$W/share" 3 5 0 "$SO_PIN" \
     | sed 's/^/    /'; then ck ok "备份导出 + 5 份分片"; else ck err "备份导出"; fi

# ---------- 运维面：整机清零 ----------
echo
echo "== [运维面 · admin] 整机清零 =="
"$ADMIN" -k "$PQCHSM_KEYSTORE" -n 2 zeroize-all | sed 's/^/    /' && ck ok "整机清零" || ck err "整机清零"

# ---------- 应用面：确认密钥没了 ----------
echo
echo "== [应用面 · PKCS#11] 确认密钥真的没了 =="
if "$PY" - "$MODULE" "$USER_PIN" <<'PYEOF' 2>/dev/null
import sys, PyKCS11
mod, user_pin = sys.argv[1:3]
lib = PyKCS11.PyKCS11Lib(); lib.load(mod)
s = lib.openSession(lib.getSlotList(True)[0], PyKCS11.CKF_SERIAL_SESSION)
s.login(user_pin)          # 清零后 PIN 也没了，这里就该失败
sys.exit(0)
PYEOF
then ck err "清零后仍能登录（不应该）"; else ck ok "清零后登录失败（密钥与 PIN 都没了）"; fi

# ---------- 运维面：恢复 ----------
echo
echo "== [运维面 · admin] 用 3 份分片恢复 =="
if "$ADMIN" -k "$PQCHSM_KEYSTORE" -n 2 restore "$W/backup.bin" \
     "$W/share.1" "$W/share.3" "$W/share.5" | sed 's/^/    /'; then
  ck ok "从备份 + 3 份分片恢复"
else ck err "恢复"; fi
# 负测试：只给 2 份必须失败
if "$ADMIN" -k "$PQCHSM_KEYSTORE" -n 2 restore "$W/backup.bin" \
     "$W/share.1" "$W/share.2" >/dev/null 2>&1; then
  ck err "只给 2 份分片竟然恢复成功（不应该）"
else ck ok "只给 2 份分片恢复失败（门限生效）"; fi
# 上面那次失败的恢复不能破坏已恢复的状态，重做一次正常恢复
"$ADMIN" -k "$PQCHSM_KEYSTORE" -n 2 restore "$W/backup.bin" \
  "$W/share.1" "$W/share.3" "$W/share.5" >/dev/null 2>&1

# ---------- 应用面：★ 最终判据 ----------
echo
echo "== [应用面 · PKCS#11] ★ 恢复后再签一次，用清零前的公钥验 =="
"$PY" - "$MODULE" "$W" "$USER_PIN" "$MSG" <<'PYEOF'
import sys, PyKCS11
mod, work, user_pin, msg = sys.argv[1:5]
CKM_ML_DSA = 0x1d
PyKCS11.CKM["CKM_ML_DSA"] = CKM_ML_DSA; PyKCS11.CKM[CKM_ML_DSA] = "CKM_ML_DSA"
lib = PyKCS11.PyKCS11Lib(); lib.load(mod)
s = lib.openSession(lib.getSlotList(True)[0], PyKCS11.CKF_SERIAL_SESSION)
s.login(user_pin)
priv = s.findObjects([(PyKCS11.CKA_CLASS, PyKCS11.CKO_PRIVATE_KEY)])[0]
pub  = s.findObjects([(PyKCS11.CKA_CLASS, PyKCS11.CKO_PUBLIC_KEY)])[0]
sig2 = bytes(s.sign(priv, msg.encode(), PyKCS11.Mechanism(CKM_ML_DSA, None)))
open(f"{work}/sig2.bin", "wb").write(sig2)
# 恢复出来的公钥必须与清零前导出的那份逐字节相同
pk_now = bytes(s.getAttributeValue(pub, [PyKCS11.CKA_VALUE])[0])
pk_before = open(f"{work}/pk.bin", "rb").read()
assert pk_now == pk_before, "恢复出来的公钥与清零前不一致"
# 并且新签名要能被它验过
assert s.verify(pub, msg.encode(), sig2, PyKCS11.Mechanism(CKM_ML_DSA, None)), "验签失败"
print(f"    恢复后签名 {len(sig2)} B，公钥与清零前逐字节相同，验签通过")
s.logout(); s.closeSession()
PYEOF
[ $? -eq 0 ] && ck ok "★ 恢复后的私钥签出的签名，被清零前的公钥验过" \
             || ck err "★ 恢复后签名验证"

echo
echo "通过 $pass，失败 $fail"
[ "$fail" -eq 0 ]
