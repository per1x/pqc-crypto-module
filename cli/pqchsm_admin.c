/* pqchsm-admin —— 管理面工具：直接对密钥库文件做备份 / 清零 / 恢复 / 审计验证
 *
 * 【为什么单独有这么一个工具】
 * PKCS#11 里**没有**备份、恢复、整机清零、审计这些操作 —— 它是应用面接口，
 * 管的是"用密钥"，不是"管密钥"。真实 HSM 也是这么分的：应用走 PKCS#11，
 * 运维走厂商自己的管理工具。
 *
 * 所以整条链是这样分工的：
 *   应用（Java/Python/pkcs11-tool）── PKCS#11 ──→ 生成密钥、签名、验签
 *   运维（本工具）────────────────  直接操作密钥库文件  ──→ 备份、清零、恢复
 * 两边共用同一个密钥库文件，**顺序访问**（不是同时）。
 * tools/e2e_p11.sh 演示的就是这个交接。
 *
 * 用法见 usage()。
 */
#include "pqchsm/anchor.h"
#include "pqchsm/audit.h"
#include "pqchsm/backup.h"
#include "pqchsm/keystore.h"
#include "pqchsm/slot.h"
#include "pqchsm/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
	fprintf(stderr,
	  "用法: pqchsm-admin -k <密钥库> [-n <槽位数>] <命令> [参数...]\n"
	  "\n"
	  "  list                                   列出各槽位状态\n"
	  "  backup <备份文件> <分片前缀> <M> <N> <slot> <so-pin>\n"
	  "                                         导出备份，分片写成 <前缀>.1 .. <前缀>.N\n"
	  "  zeroize-all                            所有槽位清零（设备级，无需会话）\n"
	  "  restore <备份文件> <分片文件...>        用给定分片恢复\n"
	  "  audit-verify <日志> [锚点 <公钥文件>]   验证审计链（给了锚点就一并验签名）\n"
	  "\n"
	  "槽位数必须与密钥库一致（默认 4）。\n");
}

static int write_file(const char *path, const uint8_t *d, size_t n)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		return -1;
	}
	int ok = fwrite(d, 1, n, f) == n;
	fclose(f);
	return ok ? 0 : -1;
}

static long read_file(const char *path, uint8_t *buf, size_t cap)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		return -1;
	}
	size_t n = fread(buf, 1, cap, f);
	fclose(f);
	return (long)n;
}

static const char *state_str(slot_state_t s)
{
	return slot_state_name(s);
}

int main(int argc, char **argv)
{
	const char *ks = NULL;
	size_t n_slots = 4;
	int i = 1;

	while (i < argc && argv[i][0] == '-') {
		if (!strcmp(argv[i], "-k") && i + 1 < argc) {
			ks = argv[++i];
		} else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			n_slots = (size_t)atoi(argv[++i]);
		} else {
			usage();
			return 2;
		}
		i++;
	}
	if (!ks || i >= argc) {
		usage();
		return 2;
	}
	const char *cmd = argv[i++];
	int rest = argc - i;

	hsm_token_t *tok = hsm_token_new(n_slots);
	if (!tok) {
		fprintf(stderr, "创建 token 失败\n");
		return 1;
	}
	/* 除了 restore（可能面对一台空白设备），其余命令都要求库能装载 */
	hsm_status_t ls = hsm_keystore_load(tok, ks);
	if (ls != HSM_OK && strcmp(cmd, "restore") != 0 && strcmp(cmd, "audit-verify") != 0) {
		fprintf(stderr, "装载密钥库失败: %s —— 槽位数对不上？(-n %zu)\n",
		        hsm_strerror(ls), n_slots);
		hsm_token_free(tok);
		return 1;
	}

	int rc = 1;

	if (!strcmp(cmd, "list")) {
		printf("%-6s %-18s %-9s %-14s %-8s %s\n",
		       "slot", "label", "state", "alg", "usage", "policy");
		for (size_t s = 0; s < n_slots; s++) {
			slot_meta_t m;
			if (hsm_slot_get_meta(tok, (hsm_slot_id_t)s, &m) != HSM_OK) {
				printf("%-6zu <元数据校验失败>\n", s);
				continue;
			}
			const pqc_alg_info_t *ai = pqc_alg_info(m.alg);
			printf("%-6zu %-18s %-9s %-14s 0x%-6x 0x%x\n", s,
			       m.label[0] ? m.label : "-", state_str(m.state),
			       ai ? ai->name : "-", m.usage, m.policy);
		}
		rc = 0;

	} else if (!strcmp(cmd, "backup") && rest == 6) {
		const char *bkfile = argv[i], *prefix = argv[i + 1];
		int M = atoi(argv[i + 2]), N = atoi(argv[i + 3]);
		hsm_slot_id_t slot = (hsm_slot_id_t)atoi(argv[i + 4]);
		const char *so_pin = argv[i + 5];
		hsm_session_t sess;
		if (hsm_session_open(tok, slot, &sess) != HSM_OK ||
		    hsm_session_login(tok, sess, HSM_ROLE_SO, so_pin) != HSM_OK) {
			fprintf(stderr, "SO 登录失败（槽位 %u）\n", slot);
			goto out;
		}
		uint8_t *shares = calloc((size_t)N, HSM_SHARE_CAP);
		size_t *lens = calloc((size_t)N, sizeof(size_t));
		size_t exported = 0;
		hsm_status_t st = hsm_backup_export(tok, sess, bkfile, (uint8_t)M, (uint8_t)N,
		                                    shares, HSM_SHARE_CAP, lens, &exported);
		if (st != HSM_OK) {
			fprintf(stderr, "备份失败: %s\n", hsm_strerror(st));
			free(shares);
			free(lens);
			goto out;
		}
		for (int k = 0; k < N; k++) {
			char p[512];
			snprintf(p, sizeof(p), "%s.%d", prefix, k + 1);
			if (write_file(p, shares + (size_t)k * HSM_SHARE_CAP, lens[k]) != 0) {
				fprintf(stderr, "写分片 %s 失败\n", p);
				free(shares);
				free(lens);
				goto out;
			}
		}
		printf("已备份 %zu 个槽位到 %s；%d 份分片写到 %s.1 .. %s.%d（门限 %d）\n",
		       exported, bkfile, N, prefix, prefix, N, M);
		pqc_secure_zero(shares, (size_t)N * HSM_SHARE_CAP);
		free(shares);
		free(lens);
		rc = 0;

	} else if (!strcmp(cmd, "zeroize-all")) {
		for (size_t s = 0; s < n_slots; s++) {
			hsm_status_t st = hsm_slot_zeroize_forced(tok, (hsm_slot_id_t)s);
			if (st != HSM_OK) {
				fprintf(stderr, "槽位 %zu 清零失败: %s\n", s, hsm_strerror(st));
				goto out;
			}
		}
		if (hsm_keystore_save(tok, ks) != HSM_OK) {
			fprintf(stderr, "清零后落盘失败\n");
			goto out;
		}
		printf("已清零全部 %zu 个槽位并落盘\n", n_slots);
		rc = 0;

	} else if (!strcmp(cmd, "restore") && rest >= 2) {
		const char *bkfile = argv[i];
		int k = rest - 1;
		uint8_t *shares = calloc((size_t)k, HSM_SHARE_CAP);
		size_t *lens = calloc((size_t)k, sizeof(size_t));
		for (int j = 0; j < k; j++) {
			long n = read_file(argv[i + 1 + j], shares + (size_t)j * HSM_SHARE_CAP,
			                   HSM_SHARE_CAP);
			if (n <= 0) {
				fprintf(stderr, "读分片 %s 失败\n", argv[i + 1 + j]);
				free(shares);
				free(lens);
				goto out;
			}
			lens[j] = (size_t)n;
		}
		size_t restored = 0;
		hsm_status_t st = hsm_backup_restore(tok, bkfile, shares, HSM_SHARE_CAP, lens,
		                                    (uint8_t)k, &restored);
		pqc_secure_zero(shares, (size_t)k * HSM_SHARE_CAP);
		free(shares);
		free(lens);
		if (st != HSM_OK) {
			fprintf(stderr, "恢复失败: %s（分片不够 M 份、分片有误或备份被改都会走到这里）\n",
			        hsm_strerror(st));
			goto out;
		}
		/* §8.4：恢复后立刻用本机 KDR 重新 sealing —— 就是存回密钥库 */
		if (hsm_keystore_save(tok, ks) != HSM_OK) {
			fprintf(stderr, "恢复后落盘失败\n");
			goto out;
		}
		printf("已用 %d 份分片恢复 %zu 个槽位，并重新 sealing 到 %s\n", k, restored, ks);
		rc = 0;

	} else if (!strcmp(cmd, "audit-verify") && rest >= 1) {
		const char *logf = argv[i];
		uint64_t bad = 0;
		if (audit_verify_file(logf, &bad) != 0) {
			fprintf(stderr, "审计链校验失败，第一处问题在第 %llu 条\n",
			        (unsigned long long)bad);
			goto out;
		}
		printf("审计链完好\n");
		if (rest >= 3 && !strcmp(argv[i + 1], "锚点")) {
			/* 兼容中文子命令写法，实际用下面的英文形式 */
		}
		if (rest >= 3) {
			const char *anc = argv[i + 1], *pkf = argv[i + 2];
			static uint8_t pk[8192];
			long n = read_file(pkf, pk, sizeof(pk));
			if (n <= 0) {
				fprintf(stderr, "读公钥 %s 失败\n", pkf);
				goto out;
			}
			uint64_t cnt = 0;
			hsm_status_t st = hsm_audit_anchor_verify(logf, anc, pk, (size_t)n, &cnt);
			if (st != HSM_OK) {
				fprintf(stderr, "锚点校验失败: %s\n", hsm_strerror(st));
				goto out;
			}
			printf("锚点校验通过，覆盖前 %llu 条\n", (unsigned long long)cnt);
		}
		rc = 0;

	} else {
		usage();
		rc = 2;
	}

out:
	hsm_token_free(tok);
	return rc;
}
