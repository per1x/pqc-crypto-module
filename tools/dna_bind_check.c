/* dna_bind_check —— 验"设备绑定"这条路在**远端主机**上真的通
 *
 *   dna_bind_check                     # 本机有 daemon 的 UNIX socket 时
 *   dna_bind_check <主机> <口令>        # 远程连板子
 *
 * ============================================================================
 * 【为什么单独有这么个工具，而不是塞进 sdf_demo】
 * ============================================================================
 * sdf_demo 是**故意不依赖 liboqs/OpenSSL** 的纯 socket 客户端（见
 * board/demo/build_client.sh 的文件头：演示的人手上未必有那套构建环境）。
 * 而验证密钥派生要调 pqc_kdr_*，那是主库、要链密码实现。把它塞进 sdf_demo
 * 会毁掉那条"一条 cc 就能编出演示客户端"的性质。所以分开。
 *
 * ============================================================================
 * 【这里验的是哪一条路】
 * ============================================================================
 * 设备绑定有两条取 DNA 的路，两条都得有人走，否则另一条就是"在源码里、
 * 永远跑不到"：
 *
 *   ① 库跑在板上   → 直接读 /dev/secmmio。**由 board/src/dna_probe 验。**
 *   ② 库跑在主机上 → DNA 经 OP_DEVICE_DNA 从 daemon 取回。**本工具验这条。**
 *
 * 真实拓扑是 ②：PKCS#11 库在主机、密码机在板上。只做 ① 的话这个功能在真实
 * 部署里等于不存在。
 *
 * ============================================================================
 * 【判据】
 * ============================================================================
 * 同一个 label、同一个盐，桩根与 DNA 根派生出来的子密钥**必须不同**。
 * keystore 拷到另一块板打不开，靠的就是这个差异。
 *
 * ⚠️ DNA 原样打印是故意的。它不是密钥，是一个公开的芯片编号 —— 有 JTAG
 *    就能读到同样的值。遮掩它反而会让人误以为派生出来的根也是秘密的。
 */
#include "pqchsm/kdr.h"
#include "sdfe.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	SDFE_HANDLE dev = NULL, ses = NULL;
	uint8_t dna[16], k_stub[32], k_dna[32];
	const pqc_kdr_provider_t *p;
	unsigned i;
	int rv, ret = 1;

	if (argc >= 3) {
		/* 端口用 0 = 走 libsdfe 的默认端口，别在这里再抄一份端口号。
		 * argv[2] 是**凭据目录**（hsm_ca.crt / client.crt / client.key），
		 * 不再是口令 —— 远程口已经换成 mTLS，见 service/pqcs_tls.h。 */
		/* 文件路径，不是密钥材料 —— 见 check_zeroize.py 的判据 */
		static char cred_ca[512], cred_crt[512], cred_key[512];
		SDFE_TLS_CREDS creds;

		snprintf(cred_ca,  sizeof cred_ca,  "%s/hsm_ca.crt", argv[2]);
		snprintf(cred_crt, sizeof cred_crt, "%s/client.crt", argv[2]);
		snprintf(cred_key, sizeof cred_key, "%s/client.key", argv[2]);
		creds.ca_file   = cred_ca;
		creds.cert_file = cred_crt;
		creds.key_file  = cred_key;
		creds.expect_cn = argc >= 4 ? argv[3] : NULL;
		rv = SDFE_OpenDeviceRemote(&dev, argv[1], 0, &creds);
	} else {
		rv = SDFE_OpenDevice(&dev);
	}
	if (rv != SDR_OK) {
		printf("连不上密码机：%s\n", SDFE_StrError(rv));
		return 1;
	}
	if (SDFE_OpenSession(dev, &ses) != SDR_OK) {
		printf("开会话失败\n");
		goto out;
	}

	rv = SDFE_GetDeviceDNA(ses, dna);
	if (rv != SDR_OK) {
		printf("取 DNA 失败：%s\n", SDFE_StrError(rv));
		printf("  —— 这块板的 BL31 白名单里可能没有 0xFFCA0050-5C 那个只读窗口\n");
		goto out;
	}
	printf("DNA = ");
	for (i = 0; i < sizeof(dna); i++) {
		printf("%02x", dna[i]);
	}
	printf("   （公开值，不是密钥）\n");

	pqc_kdr_set_provider(NULL);   /* 桩 */
	if (pqc_kdr_derive("demo/bind", NULL, 0, k_stub, sizeof(k_stub)) != 0) {
		printf("桩根派生失败\n");
		goto out;
	}
	if (pqc_kdr_install_device_dna_raw(dna, sizeof(dna)) != 0) {
		printf("安装 DNA 根失败（DNA 全 0 或全 F？）\n");
		goto out;
	}
	if (pqc_kdr_derive("demo/bind", NULL, 0, k_dna, sizeof(k_dna)) != 0) {
		printf("DNA 根派生失败\n");
		goto out;
	}
	p = pqc_kdr_get_provider();
	printf("provider = %s\n", p->name);
	printf("device_bound=%d  hardware_backed=%d   （第二个是 0：DNA 不是秘密）\n",
	       p->device_bound, p->hardware_backed);

	if (memcmp(k_stub, k_dna, sizeof(k_dna)) == 0) {
		printf("!!! 桩根与 DNA 根派生出**相同**的子密钥 —— 绑定没生效\n");
		goto out;
	}
	printf("同 label 同盐，两个根派生结果不同 —— 换板打不开，防克隆成立\n");
	ret = 0;
out:
	if (ses) {
		SDFE_CloseSession(ses);
	}
	if (dev) {
		SDFE_CloseDevice(dev);
	}
	pqc_kdr_set_provider(NULL);
	return ret;
}
