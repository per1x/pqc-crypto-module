# 两条指向仓库根的路径，都是为了"同一份东西只有一处"：
#   ../../include     PWRP 线格式的唯一定义（pqchsm/pwrp_format.h），
#                     普通世界那份实现 include 的是同一个头（见 ta_wrap.h）；
#   ../../third_party vendored 的 mlkem-native / mldsa-native。它以前在
#                     tee/ta/vendor/ 下，**只有这个 sub.mk 够得到**，于是
#                     根 CMakeLists 想复用就只能再 vendor 一份。挪到
#                     third_party/pqc-native/ 之后两套构建共用同一棵树。
global-incdirs-y += . ../include ../../include ../../third_party config

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
