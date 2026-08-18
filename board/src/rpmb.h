/* rpmb.h —— eMMC RPMB 的最小实现：读计数器 / 认证写 / 烧密钥
 *
 * 只做防回滚锚点需要的那三件事，不做通用 RPMB 存储。
 * 设计与"为什么锚在计数器上"见 include/pqchsm/rbanchor.h 的文件头。
 *
 * 所有函数返回 0 成功，负数失败；*result 回填 RPMB 的 result 字段（可为 NULL）。
 */
#ifndef PQCHSM_RPMB_H
#define PQCHSM_RPMB_H

#include <stddef.h>
#include <stdint.h>

#define RPMB_KEY_LEN   32
#define RPMB_DEV_PATH  "/dev/mmcblk0rpmb"
/* 认证密钥的存放位置。⚠️ 这块板上**没有秘密硬件根**（见 docs/SECURITY.md），
 * 所以它只能是一个 0600 的文件。这不是疏忽，是这块硬件的边界；
 * 也正因为如此，锚点必须锚在**写计数器**上而不是 RPMB 里存的某个数 ——
 * 计数器连密钥持有者都退不回去。 */
#define RPMB_KEY_PATH  "/media/sd-mmcblk1p2/hsm/pki/rpmb.key"

/* 读写计数器。**不需要密钥也能发**，但要验响应的 MAC 才能信它 ——
 * key 非 NULL 时验 MAC 与 nonce，NULL 时只取值（用于"密钥烧过没有"这种探测）。 */
int rpmb_read_counter(const char *dev, const uint8_t key[RPMB_KEY_LEN],
                      uint32_t *counter, uint16_t *result);

/* 认证写一块（256 字节）到 addr。成功后计数器 +1，*new_counter 回填新值。
 * data 可为 NULL（写全 0）—— 锚点只关心计数器，数据本身不承载语义。 */
int rpmb_write_block(const char *dev, const uint8_t key[RPMB_KEY_LEN],
                     uint16_t addr, const uint8_t *data, size_t data_len,
                     uint32_t *new_counter, uint16_t *result);

/* 烧认证密钥。⚠️ **不可逆，一块板只有一次机会。**
 * 调用方必须自己确认过这件事 —— 这里不再问第二遍。 */
int rpmb_program_key(const char *dev, const uint8_t key[RPMB_KEY_LEN],
                     uint16_t *result);

/* result 字段的人话。 */
const char *rpmb_result_str(uint16_t result);

/* 从 path 读 32 字节密钥（十六进制或裸二进制都认）。成功 0。 */
int rpmb_key_load(const char *path, uint8_t key[RPMB_KEY_LEN]);

#endif
