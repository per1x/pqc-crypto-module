/* pqchsmd —— 命令接口 daemon（TCP 传输）
 *
 * 这是一层**薄壳**：只负责 accept / 收完整一帧 / 调 pqc_proto_dispatch / 回写。
 * 所有业务逻辑都在 proto.c 里，与传输无关 —— 板子到手后把这个文件换成
 * 读写 UART 的版本即可，分派逻辑一行不改。
 *
 * ⚠️ 明文 TCP，无认证，**只绑回环**（见下面的 INADDR_LOOPBACK）。
 *    这是本机开发/测试用的接口。跨网络的那条路是 service/pqchsm_fpgad，
 *    它现在走 mTLS（见 service/README.md 与 docs/SECURITY.md）。
 *
 * 【密钥库：fail-closed —— 2026-08-18 修复，别退回去】
 * 老版本是 `if (ks && load(...) == HSM_OK) 提示一句`，失败就**继续用一个空
 * token 启动**，退出时又无条件 save 覆盖回盘。于是一次装载失败（文件损坏、
 * KDR 换了、被回滚检测拦下、甚至只是权限不对）就变成：
 *
 *     启动 → 空 token 对外服务 → 退出 → **把真正的密钥库覆盖成空的**
 *
 * 既是 fail-open（拿不到密钥库照样对外声称是密码机），又是数据销毁。
 * 现在：文件不存在（ENOENT）才新建；存在但装载失败就**拒绝启动，且绝不写盘**。
 *
 * 【安全状态同步落盘】
 * 挂上槽位层的 persist 钩子（hsm_token_set_persist_hook），PIN 失败计数、
 * 锁定、解锁、改 PIN、生成/销毁、清零这些安全状态一变就落盘。老版本只在
 * CMD_SAVE / CMD_ROTATE_KEK 时写盘，于是"试三次 PIN → 拔电 → 再试三次"
 * 能无限试下去。
 *
 * 用法：pqchsmd [-p 端口] [-s 槽位数] [-k 密钥库路径] [-1]
 *   -1  处理完一个连接就退出（给测试脚本用）
 */
#include "pqchsm/keystore.h"
#include "pqchsm/profile.h"
#include "pqchsm/proto.h"
#include "pqchsm/slot.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define RESP_CAP (1u << 21)

static volatile sig_atomic_t g_stop;

/* 槽位层的落盘钩子：安全状态一变就把整份密钥库原子写回。
 * 返回非 0 会让那次操作被判为失败（fail-closed，见 slot.h）。 */
static int persist_keystore(hsm_token_t *tok, void *user)
{
	const char *path = (const char *)user;

	return hsm_keystore_save(tok, path) == HSM_OK ? 0 : -1;
}

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

	/* 形态闸门：PRODUCTION 下没有硬件保证的 KDR 就拒绝启动；
	 * DEV 下放行，但把"信任根是个公开常量"这句话打出来。
	 * 打在最前面是有意的：它决定了后面这一切的安全含义。 */
	{
		const char *why = "";

		if (pqc_profile_startup_check(&why) != 0) {
			fprintf(stderr, "[%s] %s\n", pqc_profile_name(), why);
			return 1;
		}
		fprintf(stderr, "[%s] %s\n", pqc_profile_name(), why);
	}

	hsm_token_t *tok = hsm_token_new(n_slots);
	if (!tok) {
		fprintf(stderr, "创建 token 失败\n");
		return 1;
	}
	if (ks) {
		struct stat sb;

		if (stat(ks, &sb) == 0) {
			/* 文件在 → 必须装得进来。装不进来的原因有好几种（文件损坏、
			 * KDR 变了、防回滚锚点判定为回放、槽位数对不上），但**没有
			 * 哪一种适合"当作空库继续跑"** —— 那既是 fail-open，
			 * 也会在退出时把真库覆盖掉。 */
			hsm_status_t st = hsm_keystore_load(tok, ks);

			if (st != HSM_OK) {
				fprintf(stderr,
				        "密钥库 %s 存在但装载失败：%s\n"
				        "拒绝启动（fail-closed）。**没有写过任何东西**，"
				        "原文件完好。\n"
				        "可能原因：文件被改过 / 换了设备（KDR 不同）/ "
				        "被防回滚锚点判为回放 / -s 槽位数与库里不一致。\n",
				        ks, hsm_strerror(st));
				hsm_token_free(tok);
				return 1;
			}
			fprintf(stderr, "已载入密钥库 %s\n", ks);
		} else if (errno == ENOENT) {
			fprintf(stderr, "密钥库 %s 不存在，新建\n", ks);
		} else {
			/* stat 失败但不是"不存在"（权限、路径里有非目录……）：
			 * 同样不能猜，猜错就是拿空库覆盖一个其实存在的库。 */
			fprintf(stderr, "读不到密钥库 %s：%s —— 拒绝启动\n",
			        ks, strerror(errno));
			hsm_token_free(tok);
			return 1;
		}
		/* 到这里密钥库的状态是确定的（装载成功，或确认不存在），
		 * 才允许往盘上写。 */
		hsm_token_set_persist_hook(tok, persist_keystore, (void *)ks);
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
	/* ⚠️ 这里**故意不再** save 一次。
	 * 安全状态的每一次变化都已经由 persist 钩子当场落盘了，退出时再写一遍
	 * 没有任何新东西，却多一个"进程退出路径也能覆盖密钥库"的口子 ——
	 * 老版本正是在这里把装载失败后的空 token 写回了盘。 */
	free(req);
	free(resp);
	close(srv);
	hsm_token_free(tok);
	return 0;
}
