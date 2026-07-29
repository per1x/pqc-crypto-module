/* 内部头：只给测试用。
 *
 * pqc_wrap_with_nonce 允许指定 nonce —— 生产代码**绝不可以**调用它，
 * 它存在的唯一目的是让 tests/unit/test_wrap.c 能把"nonce 复用会怎样"
 * 演示成一条可执行的断言（§8.2 红线）。所以它不出现在公共头里。
 */
#ifndef PQCHSM_WRAP_INTERNAL_H
#define PQCHSM_WRAP_INTERNAL_H

#include "pqchsm/wrap.h"

int pqc_wrap_with_nonce(const uint8_t *kek, size_t kek_len,
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *pt, size_t pt_len,
                        const uint8_t nonce[PQC_WRAP_NONCE_LEN],
                        uint8_t *blob, size_t cap, size_t *blob_len);

#endif
