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
  $(RTL_MLDSA)/params.v \
  $(RTL_MLDSA)/mont_reduce.v \
  $(RTL_MLDSA)/reduce.v \
  $(RTL_MLDSA)/butterfly.v \
  $(RTL_MLDSA)/ntt_core.v \
  $(RTL_MLDSA)/pack.v \
  $(RTL_MLDSA)/unpack.v \
  $(RTL_MLDSA)/expand_mask.v \
  $(RTL_MLDSA)/sample_in_ball.v \
  $(RTL_MLDSA)/sampler.v \
  $(RTL_MLDSA)/keygen.v \
  $(RTL_MLDSA)/sign.v \
  $(RTL_MLDSA)/verify.v \
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
  $(RTL_BUS)/mldsa_axi.v \
  $(RTL_BUS)/sym_axi.v \
  $(RTL_BUS)/sym_vault_top.v \
  $(RTL_BOARD)/axi4lite_xbar.v \
  $(PWD)/tb_mlkem_units.v \
  $(PWD)/tb_mldsa_units.v \
  $(PWD)/tb_mldsa_sampler.v \
  $(PWD)/tb_mldsa_keygen.v \
  $(PWD)/tb_mldsa_sign.v \
  $(PWD)/tb_mldsa_verify.v \
  $(PWD)/tb_mldsa_engine.v \
  $(RTL_MLDSA)/mldsa_engine.v \
  $(PWD)/tb_trng_health.v

# 地址译码按板级顶层的槽数再跑一遍：板上现在是 7 个槽（加了 ML-DSA）。
# 不给就是模块默认的 NS=5。测试台从同名环境变量读槽数。
ifneq ($(XBAR_NS),)
COMPILE_ARGS += $(PDEF)NS=$(XBAR_NS)
export XBAR_NS
endif

# ⚠️ 这里以前按 TOPLEVEL 二选一：mldsa_axi 编行为级替身 stub_mldsa_engine.v，
#    其余编真 engine（两者同时进会重复定义 mldsa_engine）。替身已经删掉 ——
#    真 engine 落地之后再留着它，就等于把"AXI 那一层过了"说成"整条链路过了"。
#    现在 mldsa_axi 直接接真 engine，判据是 ACVP 官方向量（见 test_mldsa_axi.py）。
#
# ⚠️ 于是 mldsa_axi 这条用例也**必须带参数集**跑：engine 是编译期参数化的，
#    pset 端口对不上就拒绝启动。九格里 mldsa_axi 那一路见 tools/mldsa_grid.sh。

# ============================================================================
# 【两个仿真器的参数覆盖语法不同 —— 这里抹平】
# ============================================================================
# Icarus:    -P<顶层>.<参数>=<值>
# Verilator: -G<参数>=<值>            （不带顶层名，带了会 "Invalid option"）
#
# 为什么要支持 Verilator：**它比 Icarus 快约 7.5 倍**（实测 keygen-44：
# Icarus 42~47s / Verilator 11.6s；仿真速率 143k vs 1079k ns/s）。ML-DSA 的
# Sign 要跑满拒绝采样循环，Icarus 下 Sign-87 单格就要 36 分钟、九格全量约
# 1.8 小时，而那是每次合并的关卡。换 Verilator 后九格约 15 分钟。
#
# ⚠️ **但不要用 Verilator 当最终关卡。** 它是二值仿真、**不传播 X**，而本项目
#    踩过的第一个坑就是"空敏感列表 → 输出 X"（见 mldsa-keygen-design 的坑表
#    第 1 条）——那类 bug Icarus 会暴露，Verilator 会安静地显示 0。
#    正确分工：**开发内环用 Verilator 求快，合并/发布前用 Icarus 求准。**
ifeq ($(SIM),verilator)
PDEF := -G
# Verilator 默认把位宽告警当致命错误；仓库里 mldsa 三个模块有既存的
# WIDTHEXPAND（早于本次改动，见各自文件头），先降级以免挡住跑测试。
EXTRA_ARGS += --timing -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC
else
PDEF := -P$(TOPLEVEL).
endif

# 参数化模块（例如 mlkem_compress 的 D）用 PARAM_D 传进来，
# 覆盖顶层参数，RTL 里不留仅供测试的分支。
ifneq ($(PARAM_D),)
COMPILE_ARGS += $(PDEF)D=$(PARAM_D)
export PARAM_D
endif

# ML-DSA 参数集切换：make PARAM_K=6 PARAM_L=5 PARAM_ETA=4 …（65），
# 不给就是顶层默认的 ML-DSA-44。测试台从同名环境变量读参数集名字。
ifneq ($(PARAM_K),)
COMPILE_ARGS += $(PDEF)K=$(PARAM_K)
export PARAM_K
endif
ifneq ($(PARAM_L),)
COMPILE_ARGS += $(PDEF)L=$(PARAM_L)
export PARAM_L
endif
ifneq ($(PARAM_ETA),)
COMPILE_ARGS += $(PDEF)ETA=$(PARAM_ETA)
export PARAM_ETA
endif
# Sign/Verify 还要 τ、γ₁ 指数、γ₂ 模式、ω、β、c̃ 字节数
ifneq ($(PARAM_TAU),)
COMPILE_ARGS += $(PDEF)TAU=$(PARAM_TAU)
endif
ifneq ($(PARAM_G1LOG),)
COMPILE_ARGS += $(PDEF)G1LOG=$(PARAM_G1LOG)
endif
ifneq ($(PARAM_MODE),)
COMPILE_ARGS += $(PDEF)MODE=$(PARAM_MODE)
endif
ifneq ($(PARAM_OMG),)
COMPILE_ARGS += $(PDEF)OMG=$(PARAM_OMG)
endif
ifneq ($(PARAM_BETA),)
COMPILE_ARGS += $(PDEF)BETA=$(PARAM_BETA)
endif
ifneq ($(PARAM_CTB),)
COMPILE_ARGS += $(PDEF)CTB=$(PARAM_CTB)
endif
# engine 还要一个 PSET（0/1/2）来校验对外的 pset 端口
ifneq ($(PARAM_PSET),)
COMPILE_ARGS += $(PDEF)PSET=$(PARAM_PSET)
endif
# ⚠️ 这个 export 必须带卫兵，和上面每个 PARAM_* 一样。
# 无卫兵的 `export MLDSA_ALG` 在变量没设时会导出一个**空串**，把测试里
# os.environ.get("MLDSA_ALG", "ML-DSA-44") 的默认值顶掉 —— 于是不带任何参数
# 直接 `make MODULE=test_mldsa_keygen` 会去找一个名字为空的参数集而失败。
# 表现是"基线上就红"，很容易被当成别人改坏的。
ifneq ($(MLDSA_ALG),)
export MLDSA_ALG
endif

# mlkem_keygen 的存储观察口只在仿真里接出来（DEBUG_BANK=1）。
# 综合时它必须是 0 —— 那个口直连私钥系数。
ifeq ($(TOPLEVEL),mlkem_keygen)
COMPILE_ARGS += $(PDEF)DEBUG_BANK=1
endif
ifeq ($(TOPLEVEL),mlkem_encaps)
COMPILE_ARGS += $(PDEF)DEBUG_BANK=1
endif
ifeq ($(TOPLEVEL),mlkem_decaps)
COMPILE_ARGS += $(PDEF)DEBUG_BANK=1
endif
# 风扇的"温度陈旧就强制满速"在真硬件上要等约 0.9 秒才触发，
# 仿真里等不起 —— 把门限压到 200 拍，测的仍是同一条逻辑。
ifeq ($(TOPLEVEL),fan_ctrl)
COMPILE_ARGS += $(PDEF)STALE_LIMIT=200
# 卡死检测默认要 30000 次采样（约 30 秒），仿真里等不起，压到 20 次。
COMPILE_ARGS += $(PDEF)STUCK_LIMIT=20
endif

ifeq ($(TOPLEVEL),sysmon_drp)
# 采样间隔默认 75000 拍（1 ms），仿真里压到 200 拍；超时压到 64。
COMPILE_ARGS += $(PDEF)PERIOD=200 $(PDEF)TIMEOUT=64
endif

include $(shell cocotb-config --makefiles)/Makefile.sim
