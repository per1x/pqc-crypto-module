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
RTL_SYM       := $(PWD)/../../rtl/sym
# 板级顶层那一层里唯一的纯 RTL：地址译码。厂商原语在 zu3eg_hsm_top，
# 那个进 lint 不进 cocotb；xbar 没有原语依赖，所以能直接对拍。
RTL_BOARD     := $(PWD)/../../rtl/board
# 风扇控制在 hardware/platform/fan_ctrl/ —— 与密码核代码分开放，但同一个 bitstream
RTL_FAN       := $(PWD)/../../platform/fan_ctrl

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
  $(RTL_MLKEM)/bitpack.v \
  $(RTL_MLKEM)/keygen.v \
  $(RTL_MLKEM)/encaps.v \
  $(RTL_MLKEM)/decaps.v \
  $(RTL_MLKEM)/pack.v \
  $(RTL_MLDSA)/mont_reduce.v \
  $(RTL_MLDSA)/reduce.v \
  $(RTL_MLDSA)/butterfly.v \
  $(RTL_MLDSA)/ntt_core.v \
  $(RTL_MLDSA)/pack.v \
  $(RTL_MLDSA)/rounding.v \
  $(RTL_MLDSA)/sample.v \
  $(RTL_FAN)/fan_ctrl.v \
  $(RTL_FAN)/sysmon_drp.v \
  $(RTL_SYM)/sbox.v \
  $(RTL_SYM)/aes_core.v \
  $(RTL_SYM)/sm4_core.v \
  $(RTL_SYM)/sm3_core.v \
  $(RTL_KECCAK)/keccak_f1600.v \
  $(RTL_KECCAK)/sha3_core.v \
  $(RTL_TRNG)/trng_health.v \
  $(RTL_BUS)/axi4lite_regs.v \
  $(RTL_BUS)/axi4lite_firewall.v \
  $(RTL_BUS)/key_vault.v \
  $(RTL_BUS)/key_vault_axi.v \
  $(RTL_BUS)/pqc_accel_axi.v \
  $(RTL_BUS)/mlkem_axi.v \
  $(RTL_BUS)/sym_axi.v \
  $(RTL_BUS)/sym_vault_top.v \
  $(RTL_BOARD)/axi4lite_xbar.v \
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
ifeq ($(TOPLEVEL),mlkem_encaps)
COMPILE_ARGS += -Pmlkem_encaps.DEBUG_BANK=1
endif
ifeq ($(TOPLEVEL),mlkem_decaps)
COMPILE_ARGS += -Pmlkem_decaps.DEBUG_BANK=1
endif
# 风扇的"温度陈旧就强制满速"在真硬件上要等约 0.9 秒才触发，
# 仿真里等不起 —— 把门限压到 200 拍，测的仍是同一条逻辑。
ifeq ($(TOPLEVEL),fan_ctrl)
COMPILE_ARGS += -Pfan_ctrl.STALE_LIMIT=200
# 卡死检测默认要 30000 次采样（约 30 秒），仿真里等不起，压到 20 次。
COMPILE_ARGS += -Pfan_ctrl.STUCK_LIMIT=20
endif

ifeq ($(TOPLEVEL),sysmon_drp)
# 采样间隔默认 75000 拍（1 ms），仿真里压到 200 拍；超时压到 64。
COMPILE_ARGS += -Psysmon_drp.PERIOD=200 -Psysmon_drp.TIMEOUT=64
endif

include $(shell cocotb-config --makefiles)/Makefile.sim
