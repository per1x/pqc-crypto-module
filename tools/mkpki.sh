#!/usr/bin/env bash
# mkpki.sh —— 生成远程口（mTLS）用的一套设备 PKI
#
# ============================================================================
# 【为什么 PKI 在这台机器上生成，而不是板上】
# ============================================================================
# 板上根本没有 openssl（既没有命令行也没有共享库，rootfs 是 initramfs）。
# 更要紧的是：**CA 私钥不该在板子上**。板子是被保护的那一方，不是签发方；
# CA 私钥留在板上等于把"谁能连这台密码机"的决定权也放进了被攻击的目标里。
#
# 所以：CA 在这里生成并留在这里，板上只放**设备证书 + 设备私钥 + CA 证书**。
#
# ⚠️ 这是**原型级**的 PKI：CA 私钥就在一个 0600 的文件里，没有硬件保护、
#    没有吊销（CRL/OCSP）、没有中间 CA。够用来把"明文口令"换成"持有私钥"，
#    不够称为一套 PKI 体系。这条边界写在 docs/SECURITY.md，别在别处夸大。
#
# ============================================================================
# 用法
# ============================================================================
#   tools/mkpki.sh [输出目录] [设备CN] [客户端CN]
#
# 默认输出到 pki/（已在 .gitignore 里 —— **私钥绝不进仓库**）。
# 产物：
#   ca.key / hsm_ca.crt              CA（私钥**只留在这里**）
#   hsm_device.key / hsm_device.crt  给板子的（连同 hsm_ca.crt 一起装到板上）
#   client.key / client.crt          给操作端的（连同 hsm_ca.crt 一起放本机）
#
# 幂等：目录里已经有 CA 就复用它，只补签缺的证书 —— 重跑一次不会把已经
# 装到板上的设备证书作废（换了 CA 就等于把板子锁在门外，要重新装凭据）。
set -eu

OUT=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/pki}
DEV_CN=${2:-axu3egb-hsm-01}
CLI_CN=${3:-demo-operator}
DAYS=${DAYS:-3650}

command -v openssl >/dev/null 2>&1 || { echo "没有 openssl" >&2; exit 2; }

mkdir -p "$OUT"
chmod 700 "$OUT"
umask 077
cd "$OUT"

# ---- CA ---------------------------------------------------------------------
if [ -f ca.key ] && [ -f hsm_ca.crt ]; then
	echo "复用已有 CA（$OUT/hsm_ca.crt）"
else
	echo "生成 CA…"
	openssl ecparam -name prime256v1 -genkey -noout -out ca.key
	openssl req -x509 -new -key ca.key -sha256 -days "$DAYS" \
		-subj "/O=pqc-hsm/CN=pqc-hsm device CA" -out hsm_ca.crt
fi

# ---- 签一张证书：sign <名字前缀> <CN> <扩展段> ------------------------------
sign() {
	name=$1; cn=$2; ext=$3
	if [ -f "$name.crt" ] && [ -f "$name.key" ]; then
		echo "复用已有 $name.crt（CN=$(openssl x509 -noout -subject -in "$name.crt" | sed 's/.*CN *= *//')）"
		return
	fi
	openssl ecparam -name prime256v1 -genkey -noout -out "$name.key"
	openssl req -new -key "$name.key" -subj "/O=pqc-hsm/CN=$cn" -out "$name.csr"
	printf '%s\n' "$ext" > "$name.ext"
	openssl x509 -req -in "$name.csr" -CA hsm_ca.crt -CAkey ca.key \
		-CAcreateserial -days "$DAYS" -sha256 \
		-extfile "$name.ext" -out "$name.crt"
	rm -f "$name.csr" "$name.ext"
	echo "签发 $name.crt（CN=${cn}）"
}

# 设备证书：serverAuth。**同时给 clientAuth 是错的** —— 那会让一张被偷走的
# 设备证书可以反过来冒充操作端。两种用途分开是这套 PKI 唯一的"分级"。
sign hsm_device "$DEV_CN" \
"basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth"

# 客户端证书：clientAuth
sign client "$CLI_CN" \
"basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=clientAuth"

chmod 600 ./*.key
chmod 644 hsm_ca.crt hsm_device.crt client.crt

cat <<TXT

PKI 就绪：$OUT

  装到板子上（/media/sd-mmcblk1p2/hsm/pki）：hsm_ca.crt  hsm_device.crt  hsm_device.key
  留在操作端（~/.config/pqchsm/pki）：hsm_ca.crt  client.crt  client.key
  **ca.key 哪儿都不去**，就留在这里。

装板子与取凭据都由 tools/demo_remote.sh --provision 一条命令做掉。
TXT
