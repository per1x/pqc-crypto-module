#!/usr/bin/env bash
# tls_regress.sh —— 远程口 mTLS 的回归。**不需要板子**。
#
# 老远程口是明文 TCP + 静态口令，"回归"无从谈起：那条路本来就没有可测的
# 安全属性。换成 mTLS 之后能测的是四条**否定**性质，而否定性质才是这层的价值：
#
#   ok       本 CA 签的客户端证书 → 通，且 64 字节完整往返
#   wrongca  别家 CA 签的证书     → 拒
#   nocert   干脆不出示证书       → 拒（漏配 FAIL_IF_NO_PEER_CERT 时这条会绿）
#   acl      CN 不在白名单里      → 拒
#
# 每一轮都用**临时生成**的 PKI，跑完就删 —— 不碰任何真的凭据。
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-cc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

command -v openssl >/dev/null 2>&1 || { echo "没有 openssl，跳过"; exit 0; }

# macOS 自带的是 LibreSSL，没有完整的 TLS 1.3 API —— 用 Homebrew 的 openssl@3
SSL_CFLAGS=""; SSL_LIBS="-lssl -lcrypto"
if [ "$(uname -s)" = "Darwin" ]; then
	P=$(brew --prefix openssl@3 2>/dev/null || true)
	[ -n "$P" ] || { echo "macOS 上需要 brew install openssl@3"; exit 2; }
	SSL_CFLAGS="-I$P/include"; SSL_LIBS="-L$P/lib -lssl -lcrypto"
fi

PKI=$TMP/pki
"$ROOT/tools/mkpki.sh" "$PKI" test-device test-operator >/dev/null 2>&1

# 另起一套完全无关的 CA，签一张"看起来一样"的客户端证书 —— 用例 wrongca
ROGUE=$TMP/rogue
"$ROOT/tools/mkpki.sh" "$ROGUE" rogue-device test-operator >/dev/null 2>&1
cp "$ROGUE/client.crt" "$PKI/rogue_client.crt"
cp "$ROGUE/client.key" "$PKI/rogue_client.key"

# ACL：只放行一个**不是** test-operator 的 CN，于是 acl 用例必须被拒
printf 'somebody-else\n' > "$PKI/hsm_acl"

# shellcheck disable=SC2086
$CC -O2 -Wall -Wextra $SSL_CFLAGS -I"$ROOT/service" \
    -o "$TMP/tls_test" "$ROOT/service/tls_test.c" "$ROOT/service/pqcs_tls.c" $SSL_LIBS

fail=0
for c in ok wrongca nocert acl; do
	if "$TMP/tls_test" "$PKI" "$c" >"$TMP/$c.log" 2>&1; then
		printf '  ✓ %s\n' "$c"
	else
		printf '  ✗ %s\n' "$c"
		sed 's/^/      /' "$TMP/$c.log"
		fail=1
	fi
done

[ "$fail" = 0 ] || exit 1
echo "tls_regress: 四条全过"
