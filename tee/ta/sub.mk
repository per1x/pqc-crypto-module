global-incdirs-y += . ../include config

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
