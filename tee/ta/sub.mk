# ../../include 是仓库根的 include/ —— PWRP 线格式的**唯一**定义
# （pqchsm/pwrp_format.h）在那里，普通世界那份实现 include 的是同一个头。
# 见 ta_wrap.h 顶上的说明。
global-incdirs-y += . ../include ../../include config

srcs-y += pqchsm_ta.c
srcs-y += ta_fips202.c
srcs-y += ta_kdf.c
srcs-y += ta_wrap.c
srcs-y += ta_pqc.c
srcs-y += ta_random.c
srcs-y += ta_mlkem512.c
srcs-y += ta_mlkem768.c
srcs-y += ta_mlkem1024.c
srcs-y += ta_mldsa44.c
srcs-y += ta_mldsa65.c
srcs-y += ta_mldsa87.c

cflags-y += -DPQCHSM_TA_OPTEE=1
