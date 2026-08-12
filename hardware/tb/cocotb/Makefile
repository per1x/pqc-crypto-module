# cocotb 对拍
#
# cocotb 不支持 Vivado xsim（官方支持列表里没有它），所以工程分两层：
#   厂商中立的纯 RTL（本目录）  → Icarus/Verilator + cocotb，Python 直接读黄金向量
#   含厂商 IP 的顶层            → 该厂商自己的仿真器与 SystemVerilog testbench
# 这个分层的副作用是好的：它强迫算法核写成不依赖厂商 IP 的纯 RTL。

SIM           ?= icarus
TOPLEVEL_LANG ?= verilog
RTL_MLKEM     := $(PWD)/../../rtl/mlkem
RTL_MLDSA     := $(PWD)/../../rtl/mldsa
RTL_KECCAK    := $(PWD)/../../rtl/keccak
RTL_BUS       := $(PWD)/../../rtl/bus
RTL_TRNG      := $(PWD)/../../rtl/trng
RTL_COMMON    := $(PWD)/../../rtl/common

# 用法：make MODULE=test_ops TOPLEVEL=mont_reduce
MODULE   ?= test_ops
TOPLEVEL ?= mont_reduce

VERILOG_SOURCES = \
  $(RTL_COMMON)/ram_dp.v \
  $(RTL_MLKEM)/mont_reduce.v \
  $(RTL_MLKEM)/butterfly.v \
  $(RTL_MLKEM)/ntt_core.v \
  $(RTL_MLKEM)/basemul.v \
  $(RTL_MLKEM)/compress.v \
  $(RTL_MLKEM)/sample.v \
  $(RTL_MLKEM)/cbd_stream.v \
  $(RTL_MLKEM)/keygen.v \
  $(RTL_MLKEM)/pack.v \
  $(RTL_MLDSA)/mont_reduce.v \
  $(RTL_MLDSA)/reduce.v \
  $(RTL_MLDSA)/butterfly.v \
  $(RTL_MLDSA)/ntt_core.v \
  $(RTL_MLDSA)/rounding.v \
  $(RTL_MLDSA)/sample.v \
  $(RTL_KECCAK)/keccak_f1600.v \
  $(RTL_KECCAK)/sha3_core.v \
  $(RTL_TRNG)/trng_health.v \
  $(RTL_BUS)/axi4lite_regs.v \
  $(RTL_BUS)/pqc_accel_axi.v \
  $(PWD)/tb_mlkem_units.v \
  $(PWD)/tb_mldsa_units.v \
  $(PWD)/tb_trng_health.v

# 参数化模块（例如 mlkem_compress 的 D）用 PARAM_D 传进来，
# 由 Icarus 的 -P 覆盖顶层参数，RTL 里不留仅供测试的分支。
ifneq ($(PARAM_D),)
COMPILE_ARGS += -P$(TOPLEVEL).D=$(PARAM_D)
export PARAM_D
endif

# mlkem_keygen 的存储观察口只在仿真里接出来（DEBUG_BANK=1）。
# 综合时它必须是 0 —— 那个口直连私钥系数。
ifeq ($(TOPLEVEL),mlkem_keygen)
COMPILE_ARGS += -Pmlkem_keygen.DEBUG_BANK=1
endif

include $(shell cocotb-config --makefiles)/Makefile.sim
