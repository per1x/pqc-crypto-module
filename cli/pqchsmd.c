/* pqchsmd —— 命令接口 daemon（TCP 传输）
 *
 * 这是一层**薄壳**：只负责 accept / 收完整一帧 / 调 pqc_proto_dispatch / 回写。
 * 所有业务逻辑都在 proto.c 里，与传输无关 —— 板子到手后把这个文件换成
 * 读写 UART 的版本即可，分派逻辑一行不改。
 *
 * ⚠️ 明文 TCP，无认证。只应绑在回环地址或受控链路上；走网络必须外套 TLS。
 *
 * 用法：pqchsmd [-p 端口] [-s 槽位数] [-k 密钥库路径] [-1]
 *   -1  处理完一个连接就退出（给测试脚本用）
 */
#include "pqchsm/keystore.h"
#include "pqchsm/proto.h"
#include "pqchsm/slot.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RESP_CAP (1u << 21)

static volatile sig_atomic_t g_stop;

static void on_sig(int s)
{
	(void)s;
	g_stop = 1;
}

/* 读满 n 字节；对端关闭或出错返回 -1 */
static int read_full(int fd, uint8_t *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, buf + got, n - got);
		if (r <= 0) {
			return -1;
		}
		got += (size_t)r;
	}
	return 0;
}

static int write_full(int fd, const uint8_t *buf, size_t n)
{
	size_t done = 0;
	while (done < n) {
		ssize_t w = write(fd, buf + done, n - done);
		if (w <= 0) {
			return -1;
		}
		done += (size_t)w;
	}
	return 0;
}

static void serve_conn(int fd, pqc_proto_ctx_t *ctx, uint8_t *req, uint8_t *resp)
{
	for (;;) {
		/* 先读定长头，再按头里的长度读 payload —— UART 上也是这个逻辑 */
		if (read_full(fd, req, PQC_PROTO_HDR_LEN) != 0) {
			return;
		}
		long total = pqc_proto_frame_len(req, PQC_PROTO_HDR_LEN);
		if (total < PQC_PROTO_HDR_LEN || (size_t)total > RESP_CAP) {
			return;   /* 畸形帧：直接断开，不给试探的机会 */
		}
		if ((size_t)total > PQC_PROTO_HDR_LEN &&
		    read_full(fd, req + PQC_PROTO_HDR_LEN,
		              (size_t)total - PQC_PROTO_HDR_LEN) != 0) {
			return;
		}
		size_t rlen = 0;
		if (pqc_proto_dispatch(ctx, req, (size_t)total, resp, RESP_CAP, &rlen) != 0) {
			return;
		}
		if (write_full(fd, resp, rlen) != 0) {
			return;
		}
	}
}

int main(int argc, char **argv)
{
	int port = 9711, once = 0;
	size_t n_slots = 4;
	const char *ks = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-p") && i + 1 < argc) {
			port = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			n_slots = (size_t)atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-k") && i + 1 < argc) {
			ks = argv[++i];
		} else if (!strcmp(argv[i], "-1")) {
			once = 1;
		} else {
			fprintf(stderr, "用法: %s [-p 端口] [-s 槽位数] [-k 密钥库] [-1]\n", argv[0]);
			return 2;
		}
	}

	/* 必须用 sigaction 且**不设 SA_RESTART**：BSD/macOS 的 signal() 默认带
	 * SA_RESTART，被信号打断的 accept() 会自动重启，于是 daemon 收到 SIGTERM
	 * 后永远停在 accept 里不退出。这是实测踩到的坑，别改回 signal()。 */
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_sig;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
	}
	signal(SIGPIPE, SIG_IGN);

	hsm_token_t *tok = hsm_token_new(n_slots);
	if (!tok) {
		fprintf(stderr, "创建 token 失败\n");
		return 1;
	}
	if (ks && hsm_keystore_load(tok, ks) == HSM_OK) {
		fprintf(stderr, "已载入密钥库 %s\n", ks);
	}
	pqc_proto_ctx_t ctx = { .tok = tok, .keystore_path = ks };

	int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) {
		perror("socket");
		hsm_token_free(tok);
		return 1;
	}
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* 只绑回环：没有认证的接口不该出机器 */
	a.sin_port = htons((uint16_t)port);
	if (bind(srv, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(srv, 8) != 0) {
		perror("bind/listen");
		close(srv);
		hsm_token_free(tok);
		return 1;
	}
	fprintf(stderr, "pqchsmd 监听 127.0.0.1:%d，%zu 个槽位%s%s\n",
	        port, n_slots, ks ? "，密钥库 " : "（不落盘）", ks ? ks : "");
	fflush(stderr);

	uint8_t *req = malloc(RESP_CAP), *resp = malloc(RESP_CAP);
	if (!req || !resp) {
		fprintf(stderr, "内存不足\n");
		close(srv);
		hsm_token_free(tok);
		free(req);
		free(resp);
		return 1;
	}
	while (!g_stop) {
		int c = accept(srv, NULL, NULL);
		if (c < 0) {
			if (g_stop) {
				break;
			}
			continue;
		}
		serve_conn(c, &ctx, req, resp);
		close(c);
		if (once) {
			break;
		}
	}
	if (ks) {
		(void)hsm_keystore_save(tok, ks);
	}
	free(req);
	free(resp);
	close(srv);
	hsm_token_free(tok);
	return 0;
}
