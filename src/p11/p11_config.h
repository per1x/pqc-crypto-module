/* p11_config.h —— 引入 OASIS PKCS#11 v3.2 头文件所需的 5 个平台宏
 *
 * 官方头文件刻意不自带这些定义（见 pkcs11.h 顶部注释）：
 * 它把"指针怎么写、函数怎么导出、结构体怎么对齐"留给使用方，
 * 因为 PKCS#11 是跨 Windows/UNIX 的 ABI 契约。这里给的是典型 UNIX 定义。
 */
#ifndef PQCHSM_P11_CONFIG_H
#define PQCHSM_P11_CONFIG_H

#define CK_PTR *

#define CK_DECLARE_FUNCTION(returnType, name) \
	returnType name

/* 官方头只给声明宏，定义宏按惯例由使用方提供 */
#define CK_DEFINE_FUNCTION(returnType, name) \
	returnType name

#define CK_DECLARE_FUNCTION_POINTER(returnType, name) \
	returnType(*name)

#define CK_CALLBACK_FUNCTION(returnType, name) \
	returnType(*name)

#ifndef NULL_PTR
#define NULL_PTR 0
#endif

#include "pkcs11.h"

#endif
