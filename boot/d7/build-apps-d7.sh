#!/bin/bash
# D7：交叉编译普通世界程序。daemon = 动态 OpenSSL 1.1（板上 hsm/lib）+ 静态 libteec，
# 与批 2 那份同一链接形态（NEEDED 只有 ssl/crypto/pthread/c，RUNPATH 指 hsm/lib）。
set -e
# 与批 2 同一个编译器：Ubuntu 的 gcc-aarch64-linux-gnu 7.5（不是 Vitis 那份，后者没有静态 libc）
CC=/usr/bin/aarch64-linux-gnu-gcc
SSL=/home/build/petalinux/build/tmp/sysroots-components/aarch64/openssl
OC=/home/build/p4_optee/oc_out/export/usr
R=/home/build/pqc-fpga/d7/src
OUT=/home/build/pqc-fpga/d7/deploy
mkdir -p $OUT
cd $R/service
SSLF="-I$SSL/usr/include"
SSLL="-L$SSL/usr/lib -lssl -lcrypto -Wl,-rpath-link,$SSL/usr/lib -Wl,-rpath,/media/sd-mmcblk1p2/hsm/lib -lpthread -ldl"
$CC -O2 -Wall -Wextra $SSLF -I$OC/include -o $OUT/pqchsm_fpgad pqchsm_fpgad.c pqcs_tls.c $OC/lib/libteec.a $SSLL
$CC -O2 -Wall -Wextra $SSLF -c -o /tmp/d7_libsdfe.o libsdfe.c
$CC -O2 -Wall -Wextra $SSLF -c -o /tmp/d7_pqcs_tls.o pqcs_tls.c
/usr/bin/aarch64-linux-gnu-ar rcs /tmp/d7_libsdfe.a /tmp/d7_libsdfe.o /tmp/d7_pqcs_tls.o
$CC -O2 -Wall -Wextra $SSLF -o $OUT/sdf_demo sdf_demo.c /tmp/d7_libsdfe.a $SSLL
$CC -O2 -Wall -Wextra $SSLF -o $OUT/d7bench ../board/src/d7bench.c /tmp/d7_libsdfe.a $SSLL
$CC -O2 -Wall -Wextra $SSLF -o $OUT/hsmctl hsmctl.c /tmp/d7_libsdfe.a $SSLL
cd $R/board/src
$CC -O2 -Wall -Wextra -static -I.. -o $OUT/hsm_hwtest hsm_hwtest.c
$CC -O2 -Wall -Wextra -static -I.. -o $OUT/hsm_kem3 hsm_kem3.c
$CC -O2 -Wall -Wextra -static -o $OUT/d7probe d7probe.c
cd $R/tee/host
$CC -O2 -Wall -Wextra -static -I../include -I$OC/include -o $OUT/ta_symkey ta_symkey.c pqchsm_ta_client.c $OC/lib/libteec.a -lpthread
cp $R/tee/ta/out/4e2d9c1a-7b35-4f68-9a2c-d15e88406fa3.ta $OUT/
cp $R/board/scripts/pay_d7.sh $OUT/
cp /home/build/wdt_patch/images/BOOT_D7.BIN $OUT/
/usr/bin/aarch64-linux-gnu-readelf -d $OUT/pqchsm_fpgad | grep -E "NEEDED|RUNPATH"
ls -l $OUT; md5sum $OUT/*
echo APPS_DONE
