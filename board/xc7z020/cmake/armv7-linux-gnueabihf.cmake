# CMake 工具链文件：交叉编译到 Zynq-7000（XC7Z020）的 Cortex-A9
#
# 目标三元组 arm-linux-gnueabihf —— 32 位 ARMv7-A，硬浮点 ABI。
# 与开发机（arm64）最关键的差异是数据模型：armv7l 是 ILP32，
# long 与指针都是 4 字节，而 aarch64 是 LP64 的 8 字节。
#
# 用法：
#   cmake -S . -B build-armv7 \
#         -DCMAKE_TOOLCHAIN_FILE=board/xc7z020/cmake/armv7-linux-gnueabihf.cmake \
#         -DCMAKE_PREFIX_PATH=<liboqs 的交叉安装前缀>

set(CMAKE_SYSTEM_NAME Linux)
# 取 armv7l —— 与目标板 uname -m 的输出一致。liboqs 按这个字符串匹配
# arm32v7 架构分支，写成宽泛的 arm 会被它判成"不支持的处理器"。
set(CMAKE_SYSTEM_PROCESSOR armv7l)

# 工具链前缀可覆盖，便于换用 Xilinx/PetaLinux 自带的 SDK 工具链
set(PQCHSM_ARMV7_TRIPLE "arm-linux-gnueabihf" CACHE STRING "交叉工具链三元组")

set(CMAKE_C_COMPILER   ${PQCHSM_ARMV7_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${PQCHSM_ARMV7_TRIPLE}-g++)
set(CMAKE_AR           ${PQCHSM_ARMV7_TRIPLE}-gcc-ar)
set(CMAKE_RANLIB       ${PQCHSM_ARMV7_TRIPLE}-gcc-ranlib)

# XC7Z020 的 PS 是双核 Cortex-A9，带 NEON 与 VFPv3。
# 不要写 neon-vfpv4：那是 Cortex-A7/A15 的浮点单元，A9 上不存在。
set(PQCHSM_ARMV7_CPU "cortex-a9" CACHE STRING "目标 CPU")
set(PQCHSM_ARMV7_FPU "neon"      CACHE STRING "目标 FPU")
set(PQCHSM_ARMV7_ARCH_FLAGS
    "-mcpu=${PQCHSM_ARMV7_CPU} -mfpu=${PQCHSM_ARMV7_FPU} -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT   "${PQCHSM_ARMV7_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${PQCHSM_ARMV7_ARCH_FLAGS}")

# Debian multiarch：armhf 的库装在 /usr/lib/arm-linux-gnueabihf，
# 头文件与宿主共用 /usr/include。因此 sysroot 就是 /，
# 只需要告诉 CMake 库的架构子目录，find_library 才会先命中 armhf 那份。
set(CMAKE_LIBRARY_ARCHITECTURE ${PQCHSM_ARMV7_TRIPLE})

# 查找规则：可执行程序（cmake、python3、verilator 等构建期工具）必须用宿主的，
# 库与头文件则允许落在 sysroot 与宿主路径两边 —— multiarch 布局下二者混在一起。
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# 用 qemu-arm 直接执行交叉产物，使 ctest 无需真板即可跑。
# 宿主内核已注册 binfmt_misc 处理器时这一项是冗余的，但显式设置能让
# 没有 binfmt 的环境同样跑得起来。
find_program(PQCHSM_QEMU_ARM NAMES qemu-arm-static qemu-arm)
if(PQCHSM_QEMU_ARM)
  set(CMAKE_CROSSCOMPILING_EMULATOR ${PQCHSM_QEMU_ARM})
endif()
