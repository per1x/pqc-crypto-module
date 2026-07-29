cmake_minimum_required(VERSION 3.20)
project(pqc-hsm C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wshadow -Wvla -Wpointer-arith -Wwrite-strings)

# ---- 依赖 -------------------------------------------------------------------
# liboqs：PQC 算法（最终版 FIPS 203/204）。见 README「技术栈选择理由」。
# OpenSSL 3：AES-256-GCM 包裹、SHA3/KMAC256、安全清零。
find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)

find_path(LIBOQS_INCLUDE_DIR oqs/oqs.h HINTS /opt/homebrew/opt/liboqs/include /usr/local/include)
find_library(LIBOQS_LIBRARY NAMES oqs HINTS /opt/homebrew/opt/liboqs/lib /usr/local/lib)
if(NOT LIBOQS_INCLUDE_DIR OR NOT LIBOQS_LIBRARY)
  message(FATAL_ERROR "找不到 liboqs。macOS: brew install liboqs")
endif()
message(STATUS "liboqs: ${LIBOQS_LIBRARY}")

# ---- Verilator 仿真后端 -----------------------------------------------------
# 把 hardware/rtl/mlkem/ntt_core.v 与 hardware/rtl/keccak/keccak_f1600.v 仿真成可被 C 调用的"加速器"，
# 经 accel_transport_t 接到 pqc_accel.c 的寄存器语义上 —— 与将来真 PL 同一条路径。
# 没装 verilator 就跳过，accel_transport_verilator() 返回 NULL（如实反映）。
find_program(VERILATOR_BIN verilator)
if(VERILATOR_BIN)
  execute_process(COMMAND ${VERILATOR_BIN} --getenv VERILATOR_ROOT
                  OUTPUT_VARIABLE VERILATOR_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(VOBJ ${CMAKE_CURRENT_BINARY_DIR}/verilated)
  set(VOBJ_K ${CMAKE_CURRENT_BINARY_DIR}/verilated-keccak)
  add_custom_command(
    OUTPUT ${VOBJ}/libntt_core_lib.a ${VOBJ}/Vntt_core.h
    COMMAND ${VERILATOR_BIN} --cc --build --lib-create ntt_core_lib
            --top-module ntt_core -Mdir ${VOBJ} -Wno-fatal
            ${CMAKE_CURRENT_SOURCE_DIR}/hardware/rtl/mlkem/mont_reduce.v
            ${CMAKE_CURRENT_SOURCE_DIR}/hardware/rtl/mlkem/butterfly.v
            ${CMAKE_CURRENT_SOURCE_DIR}/hardware/rtl/mlkem/ntt_core.v
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hardware/rtl/mlkem/ntt_core.v
            ${CMAKE_CURRENT_SOURCE_DIR}/hardware/rtl/mlkem/butterfly.v
            ${CMAKE_CURRENT_SOURCE_DIR}/hardware/rtl/mlkem/mont_reduce.v
    COMMENT "Verilating hardware/rtl/mlkem/ntt_core.v")
  add_custom_target(verilate_ntt DEPENDS ${VOBJ}/libntt_core_lib.a)

  add_custom_command(
    OUTPUT ${VOBJ_K}/libkeccak_f1600_lib.a ${VOBJ_K}/Vkeccak_f1600.h
    COMMAND ${VERILATOR_BIN} --cc --build --lib-create keccak_f1600_lib
            -Mdir ${VOBJ_K} -Wno-fatal
            ${CMAKE_CURRENT_SOURCE_DIR}/hardware/rtl/keccak/keccak_f1600.v
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hardware/rtl/keccak/keccak_f1600.v
    COMMENT "Verilating hardware/rtl/keccak/keccak_f1600.v")
  add_custom_target(verilate_keccak DEPENDS ${VOBJ_K}/libkeccak_f1600_lib.a)

  add_library(pqchsm-vsim STATIC
    src/hal/verilator/ntt_sim.cpp
    src/hal/verilator/keccak_sim.cpp)
  add_dependencies(pqchsm-vsim verilate_ntt verilate_keccak)
  target_include_directories(pqchsm-vsim PRIVATE
    ${VOBJ} ${VOBJ_K} ${VERILATOR_ROOT}/include ${VERILATOR_ROOT}/include/vltstd)
  target_link_libraries(pqchsm-vsim PUBLIC
    ${VOBJ}/libntt_core_lib.a ${VOBJ_K}/libkeccak_f1600_lib.a)
  set_target_properties(pqchsm-vsim PROPERTIES CXX_STANDARD 20)
  message(STATUS "Verilator: ${VERILATOR_BIN}（RTL 仿真后端已启用）")
else()
  message(STATUS "Verilator: 未找到 —— RTL 仿真后端不编入")
endif()

# ---- 核心库 -----------------------------------------------------------------
add_library(pqchsm
  src/crypto/pqc.c
  src/crypto/pqc_liboqs.c
  src/crypto/oqs_rng.c
  src/crypto/kdf.c
  src/crypto/kdr.c
  src/hal/accel_stub.c
  src/hal/accel_verilator.c
  src/hal/pqc_accel.c
  src/proto/proto.c
  src/audit/audit.c
  src/audit/anchor.c
  src/backup/shamir.c
  src/backup/backup.c
  src/backup/inject.c
  src/store/wrap.c
  src/store/keystore.c
  src/slot/persist.c
  src/slot/fsm.c
  src/slot/meta.c
  src/slot/slot.c
  src/util/util.c
  src/util/kat.c
)
target_include_directories(pqchsm
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${LIBOQS_INCLUDE_DIR}
)
target_link_libraries(pqchsm PUBLIC OpenSSL::Crypto Threads::Threads PRIVATE ${LIBOQS_LIBRARY})
if(VERILATOR_BIN)
  target_compile_definitions(pqchsm PRIVATE PQCHSM_HAVE_VERILATOR=1)
  target_link_libraries(pqchsm PUBLIC pqchsm-vsim)
endif()

# ---- PKCS#11 v3.2 前端--------------------------------------------
# 官方 OASIS 头文件 vendored 在 third_party/，理由见那里的 README。
add_library(pqchsm-p11 SHARED src/p11/p11_module.c)
target_include_directories(pqchsm-p11 PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pkcs11-v3.2
  ${CMAKE_CURRENT_SOURCE_DIR}/src/p11)
target_link_libraries(pqchsm-p11 PRIVATE pqchsm)
set_target_properties(pqchsm-p11 PROPERTIES PREFIX "" OUTPUT_NAME "pqchsm-pkcs11")

# ---- 命令接口：daemon 与 CLI------------------------------
add_executable(pqchsmd cli/pqchsmd.c)
target_link_libraries(pqchsmd PRIVATE pqchsm)
add_executable(pqchsm-admin cli/pqchsm_admin.c)
target_link_libraries(pqchsm-admin PRIVATE pqchsm)
add_executable(pqchsm-cli cli/pqchsm_cli.c)
target_link_libraries(pqchsm-cli PRIVATE pqchsm)

# ---- 性能基线与热点采样靶子-----------------------------------------
add_executable(pqchsm-bench tools/bench.c)
target_link_libraries(pqchsm-bench PRIVATE pqchsm)

# 单次原语代价 —— 与 tools/prim_count.py 的精确计数相乘得到实测占比
add_executable(pqchsm-prim-bench tools/prim_bench.c)
target_link_libraries(pqchsm-prim-bench PRIVATE pqchsm)

# ---- fuzz 靶子-------------------------------------------------------
# 独立驱动版：不依赖 libFuzzer，接进 ctest，跑在 ASan 下。
# libFuzzer 版另见 tools/fuzz.sh（需要 brew 的 llvm，Apple clang 不带）。
add_executable(fuzz_standalone tests/fuzz/fuzz_targets.c)
target_link_libraries(fuzz_standalone PRIVATE pqchsm)
target_compile_definitions(fuzz_standalone PRIVATE FUZZ_STANDALONE)

# ---- 测试 -------------------------------------------------------------------
enable_testing()

set(PQCHSM_VECTOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vectors")

function(pqchsm_test name src)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE pqchsm)
  target_include_directories(${name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/src ${LIBOQS_INCLUDE_DIR})
  target_compile_definitions(${name} PRIVATE PQCHSM_VECTOR_DIR="${PQCHSM_VECTOR_DIR}")
endfunction()

pqchsm_test(test_util      tests/unit/test_util.c)
pqchsm_test(test_kat_parse tests/unit/test_kat_parse.c)
pqchsm_test(test_pqc_meta  tests/unit/test_pqc_meta.c)
pqchsm_test(test_pqc_roundtrip tests/unit/test_pqc_roundtrip.c)
pqchsm_test(test_kdf       tests/unit/test_kdf.c)
pqchsm_test(test_slot_fsm  tests/unit/test_slot_fsm.c)
pqchsm_test(test_slot_meta tests/unit/test_slot_meta.c)
pqchsm_test(test_slot      tests/unit/test_slot.c)
pqchsm_test(test_wrap      tests/unit/test_wrap.c)
pqchsm_test(test_keystore  tests/unit/test_keystore.c)
pqchsm_test(test_shamir    tests/unit/test_shamir.c)
pqchsm_test(test_backup    tests/unit/test_backup.c)
pqchsm_test(test_zeroize   tests/unit/test_zeroize.c)
pqchsm_test(test_audit     tests/unit/test_audit.c)
pqchsm_test(test_kdr       tests/unit/test_kdr.c)
pqchsm_test(test_proto     tests/unit/test_proto.c)
pqchsm_test(test_inject    tests/unit/test_inject.c)

# PKCS#11 测试要 dlopen 真正的 .dylib —— 测的是发布出去的那个库，
# 包括符号导出与函数表填得对不对；直接链接会绕过这些。
add_executable(test_p11 tests/unit/test_p11.c)
target_link_libraries(test_p11 PRIVATE pqchsm)
target_include_directories(test_p11 PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/tests
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pkcs11-v3.2
  ${CMAKE_CURRENT_SOURCE_DIR}/src/p11)
target_compile_definitions(test_p11 PRIVATE
  PQCHSM_P11_MODULE="$<TARGET_FILE:pqchsm-p11>")
add_dependencies(test_p11 pqchsm-p11)

pqchsm_test(test_anchor    tests/unit/test_anchor.c)
pqchsm_test(test_audit_integration tests/unit/test_audit_integration.c)
pqchsm_test(test_slot_concurrent tests/unit/test_slot_concurrent.c)
pqchsm_test(test_accel     tests/unit/test_accel.c)
pqchsm_test(test_e2e       tests/integration/test_e2e.c)
pqchsm_test(kat_runner     tests/kat/kat_runner.c)

add_test(NAME util          COMMAND test_util)
add_test(NAME kat_parse     COMMAND test_kat_parse)
add_test(NAME pqc_meta      COMMAND test_pqc_meta)
add_test(NAME pqc_roundtrip COMMAND test_pqc_roundtrip)
add_test(NAME kdf           COMMAND test_kdf)
add_test(NAME slot_fsm      COMMAND test_slot_fsm)
add_test(NAME slot_meta     COMMAND test_slot_meta)
add_test(NAME slot          COMMAND test_slot)
add_test(NAME slot_concurrent COMMAND test_slot_concurrent)
add_test(NAME wrap          COMMAND test_wrap)
add_test(NAME keystore      COMMAND test_keystore)
add_test(NAME shamir        COMMAND test_shamir)
add_test(NAME backup        COMMAND test_backup)
add_test(NAME zeroize       COMMAND test_zeroize)
add_test(NAME audit         COMMAND test_audit)
add_test(NAME kdr           COMMAND test_kdr)
add_test(NAME proto         COMMAND test_proto)
add_test(NAME inject        COMMAND test_inject)
add_test(NAME p11           COMMAND test_p11)

# pkcs11-tool 冒烟 + SoftHSMv2 行为对比。没装 OpenSC 时脚本自己 SKIP（退出 0）。
# RTL 对拍（无 cocotb/iverilog 时脚本自己 SKIP）
add_test(NAME fuzz COMMAND fuzz_standalone 4000)

find_package(Python3 COMPONENTS Interpreter)

# NTT 的独立预言机：schoolbook 负循环卷积 + 重建 ML-KEM KeyGen 比对 ACVP 向量。
# 这是"RTL 对得上真实 ML-KEM"而不只是"对得上自己的模型"的依据。
if(Python3_Interpreter_FOUND)
  add_test(NAME ntt_oracle COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware/model/ntt_oracle.py)
endif()

add_test(NAME rtl_sim COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/rtl_sim.sh)

# 原语计数的自检：海绵与 hashlib 对拍 + 计数与手推解析式对拍（含反证）
add_test(NAME prim_count COMMAND python3
         ${CMAKE_CURRENT_SOURCE_DIR}/tools/prim_count.py --self-test)

add_test(NAME cli_smoke COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/cli_smoke.sh
         ${CMAKE_CURRENT_BINARY_DIR})
# Python / Java 的 provider demo（缺 PyKCS11 或 JDK22+ 时自动 SKIP）
add_test(NAME accel         COMMAND test_accel)
add_test(NAME e2e           COMMAND test_e2e)

add_test(NAME e2e_p11 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/e2e_p11.sh
         $<TARGET_FILE:pqchsm-p11>)

add_test(NAME p11_demos COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/demo_p11.sh
         $<TARGET_FILE:pqchsm-p11>)

add_test(NAME p11_smoke COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/p11_smoke.sh
         $<TARGET_FILE:pqchsm-p11>)
add_test(NAME anchor        COMMAND test_anchor)

# 结构性回归：KDR 铁律是"只进不出"。这条用例扫公共头，确保没人加回读出根密钥的接口。
# 与 的"地址译码上物理不存在读回路径"是同一句话在软件阶段的实现。
if(Python3_Interpreter_FOUND)
  add_test(NAME kdr_no_readback COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_no_readback.py
    ${CMAKE_CURRENT_SOURCE_DIR}/include/pqchsm/kdr.h)
endif()
add_test(NAME audit_integration COMMAND test_audit_integration)

# 每类 ACVP 向量一个 ctest 用例，失败时一眼看出是哪一类
foreach(v mlkem_keygen mlkem_encaps mlkem_decaps mldsa_keygen mldsa_siggen mldsa_sigver)
  add_test(NAME kat_${v} COMMAND kat_runner ${PQCHSM_VECTOR_DIR}/${v}.kat)
endforeach()
