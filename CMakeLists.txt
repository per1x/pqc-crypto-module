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
target_link_libraries(pqchsm PUBLIC OpenSSL::Crypto PRIVATE ${LIBOQS_LIBRARY})

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
pqchsm_test(kat_runner     tests/kat/kat_runner.c)

add_test(NAME util          COMMAND test_util)
add_test(NAME kat_parse     COMMAND test_kat_parse)
add_test(NAME pqc_meta      COMMAND test_pqc_meta)
add_test(NAME pqc_roundtrip COMMAND test_pqc_roundtrip)
add_test(NAME kdf           COMMAND test_kdf)
add_test(NAME slot_fsm      COMMAND test_slot_fsm)
add_test(NAME slot_meta     COMMAND test_slot_meta)
add_test(NAME slot          COMMAND test_slot)

# 每类 ACVP 向量一个 ctest 用例，失败时一眼看出是哪一类
foreach(v mlkem_keygen mlkem_encaps mlkem_decaps mldsa_keygen mldsa_siggen mldsa_sigver)
  add_test(NAME kat_${v} COMMAND kat_runner ${PQCHSM_VECTOR_DIR}/${v}.kat)
endforeach()
