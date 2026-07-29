cmake_minimum_required(VERSION 3.20)
project(pqc-hsm C)

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

# ---- 核心库 -----------------------------------------------------------------
add_library(pqchsm
  src/crypto/pqc.c
  src/crypto/pqc_liboqs.c
  src/crypto/oqs_rng.c
  src/crypto/kdf.c
  src/crypto/kdr.c
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

# ---- PKCS#11 v3.2 前端（Phase 9）--------------------------------------------
# 官方 OASIS 头文件 vendored 在 third_party/，理由见那里的 README。
add_library(pqchsm-p11 SHARED src/p11/p11_module.c)
target_include_directories(pqchsm-p11 PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pkcs11-v3.2
  ${CMAKE_CURRENT_SOURCE_DIR}/src/p11)
target_link_libraries(pqchsm-p11 PRIVATE pqchsm)
set_target_properties(pqchsm-p11 PROPERTIES PREFIX "" OUTPUT_NAME "pqchsm-pkcs11")

# ---- 命令接口：daemon 与 CLI（Phase 5 第 3 项）------------------------------
add_executable(pqchsmd cli/pqchsmd.c)
target_link_libraries(pqchsmd PRIVATE pqchsm)
add_executable(pqchsm-cli cli/pqchsm_cli.c)
target_link_libraries(pqchsm-cli PRIVATE pqchsm)

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
add_test(NAME cli_smoke COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/cli_smoke.sh
         ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME p11_smoke COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/p11_smoke.sh
         $<TARGET_FILE:pqchsm-p11>)
add_test(NAME anchor        COMMAND test_anchor)

# 结构性回归：KDR 铁律是"只进不出"。这条用例扫公共头，确保没人加回读出根密钥的接口。
# 与 Phase 7 的"地址译码上物理不存在读回路径"是同一句话在软件阶段的实现。
find_package(Python3 COMPONENTS Interpreter)
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
