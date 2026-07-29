/* pqchsm-cli —— 命令接口客户端
 *
 * 每次调用连一次 daemon、发一条命令、打印结果。会话句柄由用户在命令行之间
 * 自己传递（daemon 持有会话状态）—— 这与 pkcs11-tool 每条命令一个进程的模型一致。
 *
 * 例：
 *   pqchsm-cli slots
 *   pqchsm-cli init-token 0 mytoken 12345678
 *   S=$(pqchsm-cli session-open 0)
 *   pqchsm-cli login $S so 12345678
 *   pqchsm-cli set-user-pin $S 1234abcd
 *   pqchsm-cli logout $S && pqchsm-cli login $S user 1234abcd
 *   H=$(pqchsm-cli generate $S ML-DSA-65 sign)
 *   pqchsm-cli pubkey $S $H > pk.bin
 *   echo -n hello | pqchsm-cli sign $S $H > sig.bin
 */
#include "pqchsm/proto.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CAP (1u << 21)

static int g_port = 9711;

static int connect_daemon(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((uint16_t)g_port);
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int rw_full(int fd, uint8_t *b, size_t n, int is_read)
{
	size_t done = 0;
	while (done < n) {
		ssize_t r = is_read ? read(fd, b + done, n - done) : write(fd, b + done, n - done);
		if (r <= 0) {
			return -1;
		}
		done += (size_t)r;
	}
	return 0;
}

/* 发一条命令并取回响应 payload。返回 status；-1 表示传输层失败。 */
static int do_cmd(uint8_t cmd, const uint8_t *payload, size_t plen,
                  uint8_t *resp, size_t resp_cap, const uint8_t **out, size_t *out_len)
{
	static uint8_t req[CAP];
	size_t rlen = 0;
	if (pqc_proto_build_req(cmd, 1, payload, plen, req, sizeof(req), &rlen) != 0) {
		return -1;
	}
	int fd = connect_daemon();
	if (fd < 0) {
		fprintf(stderr, "连不上 pqchsmd（127.0.0.1:%d）\n", g_port);
		return -1;
	}
	int rc = -1;
	if (rw_full(fd, req, rlen, 0) != 0) {
		goto out;
	}
	if (rw_full(fd, resp, PQC_PROTO_HDR_LEN, 1) != 0) {
		goto out;
	}
	{
		long total = pqc_proto_frame_len(resp, PQC_PROTO_HDR_LEN);
		if (total < PQC_PROTO_HDR_LEN || (size_t)total > resp_cap) {
			goto out;
		}
		if ((size_t)total > PQC_PROTO_HDR_LEN &&
		    rw_full(fd, resp + PQC_PROTO_HDR_LEN,
		            (size_t)total - PQC_PROTO_HDR_LEN, 1) != 0) {
			goto out;
		}
		uint16_t st = 0;
		if (pqc_proto_resp_status(resp, (size_t)total, &st, out, out_len) != 0) {
			goto out;
		}
		rc = (int)st;
	}
out:
	close(fd);
	return rc;
}

static pqc_alg_t alg_by_name(const char *s)
{
	pqc_alg_t a = pqc_alg_by_name(s);
	return a;
}

static uint32_t usage_by_name(const char *s)
{
	if (!strcmp(s, "sign")) {
		return KEY_USAGE_SIGN;
	}
	if (!strcmp(s, "verify")) {
		return KEY_USAGE_VERIFY;
	}
	if (!strcmp(s, "decap")) {
		return KEY_USAGE_DECAP;
	}
	if (!strcmp(s, "encap")) {
		return KEY_USAGE_ENCAP;
	}
	return 0;
}

static const char *state_name(uint32_t s)
{
	return slot_state_name((slot_state_t)s);
}

static void usage(void)
{
	fprintf(stderr,
	  "用法: pqchsm-cli [-p 端口] <命令> [参数...]\n"
	  "  ping\n"
	  "  slots                                   列出槽位数\n"
	  "  info <slot>                             槽位信息\n"
	  "  init-token <slot> <label> <so-pin>\n"
	  "  session-open <slot>                     打印会话句柄\n"
	  "  session-close <sess>\n"
	  "  login <sess> so|user <pin>\n"
	  "  logout <sess>\n"
	  "  set-user-pin <sess> <pin>\n"
	  "  generate <sess> <alg> <sign|decap> [policy]   打印对象句柄\n"
	  "  pubkey <sess> <handle>                  公钥写到 stdout\n"
	  "  sign <sess> <handle>                    待签数据从 stdin，签名到 stdout\n"
	  "  destroy <sess> <handle>\n"
	  "  zeroize <sess> <slot>\n"
	  "  unlock <sess> <slot>\n"
	  "  save | rotate-kek\n");
}

int main(int argc, char **argv)
{
	int argi = 1;
	if (argc > 2 && !strcmp(argv[1], "-p")) {
		g_port = atoi(argv[2]);
		argi = 3;
	}
	if (argi >= argc) {
		usage();
		return 2;
	}
	const char *cmd = argv[argi++];
	int rest = argc - argi;

	static uint8_t pl[CAP], resp[CAP];
	tlv_writer_t w;
	tlv_init(&w, pl, sizeof(pl));
	const uint8_t *out = NULL;
	size_t out_len = 0;
	int st = -1;

	if (!strcmp(cmd, "ping")) {
		st = do_cmd(CMD_PING, pl, 0, resp, sizeof(resp), &out, &out_len);
	} else if (!strcmp(cmd, "slots")) {
		st = do_cmd(CMD_SLOT_LIST, pl, 0, resp, sizeof(resp), &out, &out_len);
		if (st == HSM_OK) {
			uint64_t c = 0;
			tlv_get_u64(out, out_len, TAG_COUNT, &c);
			printf("%llu\n", (unsigned long long)c);
		}
	} else if (!strcmp(cmd, "info") && rest == 1) {
		tlv_put_u32(&w, TAG_SLOT, (uint32_t)strtoul(argv[argi], NULL, 0));
		st = do_cmd(CMD_SLOT_INFO, pl, w.len, resp, sizeof(resp), &out, &out_len);
		if (st == HSM_OK) {
			uint32_t alg = 0, state = 0, us = 0, po = 0;
			uint64_t uc = 0;
			size_t ll = 0;
			const uint8_t *lab = tlv_find(out, out_len, TAG_LABEL, &ll);
			tlv_get_u32(out, out_len, TAG_ALG, &alg);
			tlv_get_u32(out, out_len, TAG_STATE, &state);
			tlv_get_u32(out, out_len, TAG_USAGE, &us);
			tlv_get_u32(out, out_len, TAG_POLICY, &po);
			tlv_get_u64(out, out_len, TAG_COUNT, &uc);
			const pqc_alg_info_t *ai = pqc_alg_info((pqc_alg_t)alg);
			printf("label=%.*s state=%s alg=%s usage=0x%x policy=0x%x use_count=%llu\n",
			       (int)ll, lab ? (const char *)lab : "", state_name(state),
			       ai ? ai->name : "-", us, po, (unsigned long long)uc);
		}
	} else if (!strcmp(cmd, "init-token") && rest == 3) {
		tlv_put_u32(&w, TAG_SLOT, (uint32_t)strtoul(argv[argi], NULL, 0));
		tlv_put(&w, TAG_LABEL, argv[argi + 1], strlen(argv[argi + 1]));
		tlv_put(&w, TAG_PIN, argv[argi + 2], strlen(argv[argi + 2]));
		st = do_cmd(CMD_INIT_TOKEN, pl, w.len, resp, sizeof(resp), &out, &out_len);
	} else if (!strcmp(cmd, "session-open") && rest == 1) {
		tlv_put_u32(&w, TAG_SLOT, (uint32_t)strtoul(argv[argi], NULL, 0));
		st = do_cmd(CMD_SESSION_OPEN, pl, w.len, resp, sizeof(resp), &out, &out_len);
		if (st == HSM_OK) {
			uint64_t s = 0;
			tlv_get_u64(out, out_len, TAG_SESSION, &s);
			printf("%llu\n", (unsigned long long)s);
		}
	} else if (!strcmp(cmd, "session-close") && rest == 1) {
		tlv_put_u64(&w, TAG_SESSION, strtoull(argv[argi], NULL, 0));
		st = do_cmd(CMD_SESSION_CLOSE, pl, w.len, resp, sizeof(resp), &out, &out_len);
	} else if (!strcmp(cmd, "login") && rest == 3) {
		tlv_put_u64(&w, TAG_SESSION, strtoull(argv[argi], NULL, 0));
		tlv_put_u32(&w, TAG_ROLE,
		            !strcmp(argv[argi + 1], "so") ? HSM_ROLE_SO : HSM_ROLE_USER);
		tlv_put(&w, TAG_PIN, argv[argi + 2], strlen(argv[argi + 2]));
		st = do_cmd(CMD_LOGIN, pl, w.len, resp, sizeof(resp), &out, &out_len);
	} else if (!strcmp(cmd, "logout") && rest == 1) {
		tlv_put_u64(&w, TAG_SESSION, strtoull(argv[argi], NULL, 0));
		st = do_cmd(CMD_LOGOUT, pl, w.len, resp, sizeof(resp), &out, &out_len);
	} else if (!strcmp(cmd, "set-user-pin") && rest == 2) {
		tlv_put_u64(&w, TAG_SESSION, strtoull(argv[argi], NULL, 0));
		tlv_put(&w, TAG_PIN, argv[argi + 1], strlen(argv[argi + 1]));
		st = do_cmd(CMD_SET_USER_PIN, pl, w.len, resp, sizeof(resp), &out, &out_len);
	} else if (!strcmp(cmd, "generate") && (rest == 3 || rest == 4)) {
		pqc_alg_t a = alg_by_name(argv[argi + 1]);
		uint32_t u = usage_by_name(argv[argi + 2]);
		if (a == PQC_ALG_NONE || u == 0) {
			fprintf(stderr, "算法或用途不认识\n");
			return 2;
		}
		tlv_put_u64(&w, TAG_SESSION, strtoull(argv[argi], NULL, 0));
		tlv_put_u32(&w, TAG_ALG, (uint32_t)a);
		tlv_put_u32(&w, TAG_USAGE, u);
		if (rest == 4) {
			tlv_put_u32(&w, TAG_POLICY, (uint32_t)strtoul(argv[argi + 3], NULL, 0));
		}
		st = do_cmd(CMD_GENERATE, pl, w.len, resp, sizeof(resp), &out, &out_len);
		if (st == HSM_OK) {
			uint64_t h = 0;
			tlv_get_u64(out, out_len, TAG_HANDLE, &h);
			printf("%llu\n", (unsigned long long)h);
		}
	} else if ((!strcmp(cmd, "pubkey") || !strcmp(cmd, "sign")) && rest == 2) {
		int is_sign = !strcmp(cmd, "sign");
		tlv_put_u64(&w, TAG_SESSION, strtoull(argv[argi], NULL, 0));
		tlv_put_u64(&w, TAG_HANDLE, strtoull(argv[argi + 1], NULL, 0));
		if (is_sign) {
			static uint8_t data[1 << 16];
			size_t n = fread(data, 1, sizeof(data), stdin);
			tlv_put(&w, TAG_DATA, data, n);
		}
		st = do_cmd(is_sign ? CMD_SIGN : CMD_PUBKEY, pl, w.len, resp, sizeof(resp),
		            &out, &out_len);
		if (st == HSM_OK) {
			size_t l = 0;
			const uint8_t *v = tlv_find(out, out_len, is_sign ? TAG_SIG : TAG_DATA, &l);
			if (v) {
				fwrite(v, 1, l, stdout);
			}
		}
	} else if (!strcmp(cmd, "destroy") && rest == 2) {
		tlv_put_u64(&w, TAG_SESSION, strtoull(argv[argi], NULL, 0));
		tlv_put_u64(&w, TAG_HANDLE, strtoull(argv[argi + 1], NULL, 0));
		st = do_cmd(CMD_DESTROY, pl, w.len, resp, sizeof(resp), &out, &out_len);
	} else if ((!strcmp(cmd, "zeroize") || !strcmp(cmd, "unlock")) && rest == 2) {
		tlv_put_u64(&w, TAG_SESSION, strtoull(argv[argi], NULL, 0));
		tlv_put_u32(&w, TAG_SLOT, (uint32_t)strtoul(argv[argi + 1], NULL, 0));
		st = do_cmd(!strcmp(cmd, "zeroize") ? CMD_ZEROIZE : CMD_UNLOCK,
		            pl, w.len, resp, sizeof(resp), &out, &out_len);
	} else if (!strcmp(cmd, "save")) {
		st = do_cmd(CMD_SAVE, pl, 0, resp, sizeof(resp), &out, &out_len);
	} else if (!strcmp(cmd, "rotate-kek")) {
		st = do_cmd(CMD_ROTATE_KEK, pl, 0, resp, sizeof(resp), &out, &out_len);
	} else {
		usage();
		return 2;
	}

	if (st < 0) {
		return 1;
	}
	if (st != HSM_OK) {
		fprintf(stderr, "错误: %s (%d)\n", hsm_strerror((hsm_status_t)st), st);
		return 1;
	}
	return 0;
}
