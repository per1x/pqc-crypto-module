#!/bin/bash
# D7 位流：dev（演示形态）+ sec（送检形态），与批 1/2 的 bitbuild.sh 同一配方
source /tools/Xilinx/Vivado/2020.1/settings64.sh
cd /home/build/pqc-fpga/d7/hardware_tmp || exit 1
set -x
PQC_DEV_OPEN=1 vivado -mode batch -nojournal -log dev.log -source syn/impl_bitstream.tcl > dev.stdout 2>&1
echo "DEV_RC=$?"
vivado -mode batch -nojournal -log sec.log -source syn/impl_bitstream.tcl > sec.stdout 2>&1
echo "SEC_RC=$?"
ls -la syn/impl/*.bit
md5sum syn/impl/*.bit
echo BITBUILD_DONE
