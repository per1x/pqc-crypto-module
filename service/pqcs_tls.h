/* pqcs_tls.h —— 远程口的传输安全层：**双向 TLS（mTLS）**
 *
 * ============================================================================
 * 【它替掉了什么，以及为什么非换不可】
 * ============================================================================
 * 老的远程口是**明文 TCP + 一条静态口令**：
 *
 *     客户端 ──TCP──▶ daemon        第一帧 OP_AUTH，载荷 = 预共享口令
 *
 * 三个问题，每一个单独都足以让这条口不能出实验室：
 *
 *   ① **口令在线上是明文的。** 同网段抓一次包就拿到它，而它是静态的 ——
 *      拿到即永久。ct_eq 那种常量时间比较在这里毫无意义：攻击者根本不需要猜。
 *   ② **整条会话都是明文的。** 公钥、密文、签名、明文数据全在网上裸奔，
 *      而且可以被**改**（没有任何完整性保护）。中间人可以把一次验签的
 *      结果从"不通过"改成"通过"。
 *   ③ **可重放。** 认证帧本身可以录下来重放，服务端没有任何新鲜性判据。
 *
 * 现在这条口是 mTLS：
 *
 *   · 设备有自己的证书与私钥（device cert），客户端**必须验它** ——
 *     ② 里的中间人因此不成立；
 *   · 客户端也必须出示证书，由同一个设备 CA 签发（SSL_VERIFY_PEER |
 *     SSL_VERIFY_FAIL_IF_NO_PEER_CERT）—— 身份从"知道一个字符串"变成
 *     "持有一把私钥"，抓包拿不到它；
 *   · TLS 1.3 强制 ECDHE，每次会话密钥都是新的 —— ③ 里的重放不成立，
 *     录下来的握手换一个会话就是废字节；
 *   · 记录层自带 AEAD，①② 的机密性与完整性一起解决。
 *
 * 也就是说"防重放的 nonce"不需要单独发明：TLS 1.3 的握手本身就是那个 nonce。
 * 这一点写在这里，免得有人回头又加一层自制的挑战应答。
 *
 * ============================================================================
 * 【最小版本与算法：TLS 1.3 only】
 * ============================================================================
 * 两侧的实现都在这个仓库里、同时部署，没有任何要兼容老客户端的理由。
 * 那就把版本下限直接顶到 1.3：
 *   · 不需要判断哪些 1.2 套件是安全的（那张表每年都要重新看一遍）；
 *   · 没有可降级的空间 —— 降级攻击的前提是存在一个更弱的选项。
 *
 * ============================================================================
 * 【ACL 与限速】
 * ============================================================================
 * 证书链验过只说明"这是我们签发的一张证书"。谁能用哪台密码机是另一件事，
 * 所以再加一层 CN 白名单（pqcs_tls_acl_allows）。名单文件不存在时放行任何
 * 由本 CA 签发的证书 —— 这是**有意的默认**：演示形态下只签了一张客户端证书，
 * 要求同时维护一份名单只会让人把 CA 私钥拿去乱签。这条取舍写在 SECURITY.md。
 *
 * 限速挡的是"拿一堆无效证书猛敲"：握手是要做非对称运算的，不限速的话
 * 一台机器就能把单线程的 daemon 占满（可用性问题，不是机密性问题）。
 */
#ifndef PQCHSM_PQCS_TLS_H
#define PQCHSM_PQCS_TLS_H

#include <openssl/ssl.h>
#include <stddef.h>

/* 默认的凭据位置。板上在 SD 的 p2（与 hsm-boot.sh 同一个目录）。 */
#define PQCS_PKI_DIR      "/media/sd-mmcblk1p2/hsm/pki"
#define PQCS_CA_FILE      PQCS_PKI_DIR "/hsm_ca.crt"
#define PQCS_DEV_CERT     PQCS_PKI_DIR "/hsm_device.crt"
#define PQCS_DEV_KEY      PQCS_PKI_DIR "/hsm_device.key"
#define PQCS_ACL_FILE     PQCS_PKI_DIR "/hsm_acl"

/* 建服务端 SSL_CTX。要求对端出示由 ca_file 签发的证书。
 * 失败返回 NULL，并把原因写进 err（可为 NULL）。 */
SSL_CTX *pqcs_tls_server_ctx(const char *ca_file, const char *cert_file,
                             const char *key_file, char *err, size_t errcap);

/* 建客户端 SSL_CTX。同样要求验对端（板子）的证书。 */
SSL_CTX *pqcs_tls_client_ctx(const char *ca_file, const char *cert_file,
                             const char *key_file, char *err, size_t errcap);

/* 取对端证书的 CN，写进 out（NUL 结尾）。没有对端证书或没有 CN 返回 -1。 */
int pqcs_tls_peer_cn(SSL *ssl, char *out, size_t cap);

/* CN 是否在白名单里。
 * acl_file 不存在 → 返回 1（放行；见文件头对这个默认的解释）。
 * 存在但 CN 不在里面 → 返回 0。 */
int pqcs_tls_acl_allows(const char *acl_file, const char *cn);

/* 读满 / 写满。返回 0 成功，-1 失败（含对端关闭）。 */
int pqcs_tls_read_all(SSL *ssl, void *buf, size_t n);
int pqcs_tls_write_all(SSL *ssl, const void *buf, size_t n);

/* 把最近一条 OpenSSL 错误取成一行人话（队列会被清空）。 */
void pqcs_tls_last_error(char *out, size_t cap);

#endif
