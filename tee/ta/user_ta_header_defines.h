/* user_ta_header_defines.h —— TA 元数据（UUID/栈/堆/标志）
 *
 * ML-DSA-87 签名的栈用量大（ref 后端 rejection loop 里的 polyvec 缓冲），
 * 栈给 512KB；堆 4MB 足够（当前实现不用 TEE_Malloc，留余量给后续）。
 * TZDRAM 共 256MB，单个 TA 这点开销可忽略。
 */
#ifndef USER_TA_HEADER_DEFINES_H
#define USER_TA_HEADER_DEFINES_H

#include "pqchsm_ta_proto.h"

#define TA_UUID TA_PQCHSM_UUID

#define TA_FLAGS (TA_FLAG_SINGLE_INSTANCE | TA_FLAG_MULTI_SESSION)

#define TA_STACK_SIZE (512 * 1024)
#define TA_DATA_SIZE  (4 * 1024 * 1024)

#define TA_VERSION     "1.0"
#define TA_DESCRIPTION "pqc-hsm key operations TA (ML-KEM/ML-DSA in S-EL1)"

#endif /* USER_TA_HEADER_DEFINES_H */
