# cocotb 对拍（路线图 §5.3.3）
#
# ⚠️ §5.3.1 的坑：**cocotb 不支持 Vivado xsim**（官方支持列表里就没有）。
# 所以工程分两层：
#   自研纯 RTL（本目录）      → Icarus/Verilator + cocotb，Python 直接读黄金向量
#   含 Vivado IP 的顶层       → xsim + SystemVerilog testbench（另建目录）
# 这个分层的副作用是好的：它强迫算法核写成不依赖厂商 IP 的纯 RTL。

SIM           ?= icarus
TOPLEVEL_LANG ?= verilog
RTL           := $(PWD)/../../rtl/mlkem

# 用法：make MODULE=test_ops TOPLEVEL=mont_reduce
MODULE   ?= test_ops
TOPLEVEL ?= mont_reduce

VERILOG_SOURCES = $(RTL)/mont_reduce.v $(RTL)/butterfly.v $(RTL)/ntt_core.v

include $(shell cocotb-config --makefiles)/Makefile.sim
