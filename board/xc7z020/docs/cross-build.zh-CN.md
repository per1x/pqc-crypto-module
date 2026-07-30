[English](cross-build.md) · **中文**

# 交叉编译到 Zynq-7000（XC7Z020）

XC7Z020 的处理系统是双核 Cortex-A9：32 位 ARMv7-A，带 NEON 与 VFPv3，
运行硬浮点 Linux 用户态（`arm-linux-gnueabihf`）。本项目的开发机是 64 位
arm64。两者的数据模型不同，这份文档存在的理由就是这个差异：

| | 开发机（aarch64） | 目标板（armv7l） |
|---|---|---|
| 数据模型 | LP64 | ILP32 |
| `long`、`unsigned long` | 8 字节 | **4 字节** |
| 指针、`size_t`、`ptrdiff_t` | 8 字节 | **4 字节** |
| `off_t`（glibc 默认） | 8 字节 | **4 字节** |
| `time_t`（glibc 默认） | 8 字节 | **4 字节** |
| `long long`、`uint64_t` | 8 字节 | 8 字节 |

凡是假设 `long` 或指针能装下 64 位值的代码，在开发机上编得过、测得过，
只有到目标板上才会坏。`board/xc7z020/tools/armv7_test.sh` 的作用就是在板子
还没进入回路之前把这个缺口补上。

## 工具链与依赖

除 liboqs 之外，Debian multiarch 提供了全部依赖：

```sh
dpkg --add-architecture armhf
apt-get update
apt-get install -y \
    crossbuild-essential-armhf \   # arm-linux-gnueabihf 的 gcc/g++ 与 armhf libc
    qemu-user-static \             # qemu-arm-static，用来执行交叉产物
    libssl-dev:armhf \            # 目标侧的 OpenSSL 3
    cmake ninja-build curl python3
```

`libssl-dev:armhf` 的库装在 `/usr/lib/arm-linux-gnueabihf`，头文件与宿主的包
共用 `/usr/include`。也就是说 sysroot 就是 `/`，不是单独一棵树 ——
这正是工具链文件设 `CMAKE_LIBRARY_ARCHITECTURE` 而不是 `CMAKE_SYSROOT` 的原因。

liboqs 没有 armhf 的 Debian 包，必须用同一个工具链文件从源码交叉编译。

## CMake 工具链文件

即 `board/xc7z020/cmake/armv7-linux-gnueabihf.cmake`。其中三处不是套话：

- **`CMAKE_SYSTEM_PROCESSOR` 取 `armv7l`，不是 `arm`。** liboqs 用
  `armel|armhf|armv7|arm32v7` 匹配这个字符串来选它的 32 位 ARM 架构分支；
  写成宽泛的 `arm` 会让它以「Unknown or unsupported processor」配置失败。
- **`-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard`。** 不能写 `neon-vfpv4`
  —— 那个浮点单元属于 Cortex-A7/A15，A9 上不存在。
- **`CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER`，其余为 `BOTH`。**
  构建期工具（`cmake`、`python3`、`verilator`）必须是宿主二进制，
  而库与头文件在 multiarch 布局下本来就合法地位于宿主路径。

检测到 `qemu-arm-static` 时它还会设 `CMAKE_CROSSCOMPILING_EMULATOR`，
使 `ctest` 能直接运行交叉出来的可执行文件。

CPU、FPU 与三元组都是 cache 变量，因此换用 PetaLinux 或 Xilinx SDK 的工具链
不需要改文件：

```sh
cmake -S . -B build-armv7 \
      -DCMAKE_TOOLCHAIN_FILE=board/xc7z020/cmake/armv7-linux-gnueabihf.cmake \
      -DPQCHSM_ARMV7_TRIPLE=arm-xilinx-linux-gnueabi \
      -DCMAKE_PREFIX_PATH=/path/to/liboqs-armv7
```

## 交叉编译 liboqs

只编本项目用到的六个参数集，构建时间远小于一分钟：

```sh
cmake -S liboqs -B liboqs/build -GNinja \
  -DCMAKE_TOOLCHAIN_FILE=board/xc7z020/cmake/armv7-linux-gnueabihf.cmake \
  -DCMAKE_BUILD_TYPE=Release -DOQS_BUILD_ONLY_LIB=ON -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX=/opt/oqs-armv7 \
  -DOQS_MINIMAL_BUILD="KEM_ml_kem_512;KEM_ml_kem_768;KEM_ml_kem_1024;SIG_ml_dsa_44;SIG_ml_dsa_65;SIG_ml_dsa_87"
ninja -C liboqs/build install
```

liboqs 会选 `arm32v7` 架构分支，编出可移植的 C 参考实现；liboqs 0.16.0 里
ML-KEM 与 ML-DSA 都没有 ARM32 汇编路径，所以目标板拿到的与宿主参考构建是同一份代码。

## 构建与测试本工程

```sh
cmake -S . -B build-armv7 -GNinja \
      -DCMAKE_TOOLCHAIN_FILE=board/xc7z020/cmake/armv7-linux-gnueabihf.cmake \
      -DCMAKE_PREFIX_PATH=/opt/oqs-armv7
ninja -C build-armv7
ctest --test-dir build-armv7 --output-on-failure
```

`board/xc7z020/tools/armv7_test.sh` 在 `linux/arm64` 的 Debian 容器里从零做完
上述全部步骤，并把它实际构建所用的类型宽度打印出来。没有 Docker 时它干净地
SKIP（退出 0）；宿主 `127.0.0.1:$PQCHSM_PROXY_PORT`（默认 6152）在监听时，
它把宿主代理透进容器。

### 为什么是交叉编译，而不是模拟容器

`tools/aarch64_test.sh` 能用原生 `linux/arm64` 容器，是因为开发机本身就是
arm64。armv7 没有这个选项：Apple Silicon 不实现 AArch32 执行状态，
32 位 ARM 代码在这台机器上只能模拟。把本工程连 liboqs 一起放进模拟容器里编译，
比「交叉编译 + 只模拟测试」慢一个数量级，后者就是脚本采用的做法。

### QEMU 覆盖到什么，以及边界

内核为 `qemu-arm` 注册了 `binfmt_misc` 处理器时，armv7 二进制可以透明执行，
整套 `ctest` 都能跑，包括由 shell 脚本驱动的用例（`cli_smoke`、`e2e_p11`、
`p11_demos`）以及 `dlopen` PKCS#11 动态库的那一条。没有 `binfmt_misc` 时，
`CMAKE_CROSSCOMPILING_EMULATOR` 仍能覆盖所有以可执行目标为命令的用例，
但 shell 脚本自己 exec 二进制时拿不到模拟器前缀；脚本会排除这些用例并说明原因。

有两类结果不能从 QEMU 推到板子上：

- **时序。** `ct_timing` 是对实测执行时间做 Welch t 检验。在模拟下它验证的是
  统计机制与对照组的行为是否正确，而不是目标硬件是否常量时间。
  关于 Cortex-A9 的常量时间结论必须在板上测量。
- **性能。** QEMU 下的 `pqchsm-bench` 数字与 A9 无关。

其余部分 —— 数据模型、结构体布局、glibc 行为、系统调用语义、`dlopen`
与符号导出 —— 都被如实地跑到了。

## 目标板上的部署

### 类型宽度必须与构建一致

本工程在 32 位目标上定义 `_FILE_OFFSET_BITS=64`，并在 C 库支持时定义
`_TIME_BITS=64`（见 `CMakeLists.txt`）。这两个宏改变了出现在函数签名里的
类型大小，因此必须对每一个翻译单元一致地定义 —— 包括任何链接 `libpqchsm`
的外部代码。混用会造成 ABI 不匹配，而且没有任何编译告警能查出来。

### PYNQ

PYNQ-Z2 的 PYNQ 镜像是基于 Ubuntu 的 armhf。装上 `libssl-dev` 与一份交叉或
本机编出的 liboqs，然后在板上构建，或把交叉构建的产物拷过去。DMA 缓冲来自
`pynq.allocate`，它返回的是 `dma-coherent`（非缓存）缓冲，物理地址只有运行时
才知道 —— 那条路径走 Python，不经过 `board/xc7z020/src/accel_zynq.c`。

### PetaLinux

覆盖 `PQCHSM_ARMV7_TRIPLE` 来使用 SDK 的 `arm-*-linux-gnueabihf` 工具链。
有两件依赖设备树的事必须与 `board/xc7z020/include/pqc_accel_zynq.h` 对上：

- 绑给 `generic-uio` 的 PL 地址段，使 `/sys/class/uio/uioN/maps` 报出的地址
  正是头文件期望的那些；
- 支撑 `PQC_ZYNQ_DMA_BUF_PHYS` 的 `reserved-memory` 节点，它必须物理连续，
  因为 AXI-DMA 看到的是物理地址。

### Cache 一致性

PS7 的 HP 口与 Cortex-A9 的 L1/L2 不保证一致。DMA 缓冲必须是非缓存映射；
`accel_zynq.c` 用 `O_SYNC` 打开 `/dev/mem` 正是为此。这一点弄错的表现是
间歇性的错误结果，而不是干净的失败。

### 根文件系统

在目标板上跑 `ctest` 还需要 `python3` 来执行结构性检查（`ct_audit`、
`zeroize_check`、`prim_count`、`kdr_no_readback`）。这些检查扫的是源码文本，
与架构无关，因此同样可以在宿主上跑；最小化的目标 rootfs 不需要 Python。

## 32 位与 64 位的行为差异：结论

### 扫描范围与方法

对 `src/`、`cli/`、`tests/`、`tools/`、`include/`、`board/xc7z020/src/`、
`board/xc7z020/tests/` 下的全部 C 源码检查了以下几类：

- 关于 `sizeof(long)`、`sizeof(size_t)`、`sizeof(void *)` 的假设；
- `printf`/`scanf` 的格式串与其实参类型是否相符；
- 指针与整数的互转；
- `time_t` 与 `off_t` 的用法；
- 序列化中关于结构体填充与对齐的假设；
- 64 位整数的对齐要求；
- PKCS#11 前端里 `CK_ULONG` 的宽度。

机械手段上，用
`-Wformat=2 -Wconversion -Wsign-conversion -Wpointer-to-int-cast -Wint-to-pointer-cast -Wshift-count-overflow -Wshift-overflow=2 -Wpadded`
重跑了一遍交叉构建，并逐条甄别了所有诊断。这些开关**刻意不**写进
`CMakeLists.txt`：`-Wconversion` 与 `-Wpadded` 会在大量正确且有意为之的代码上
报警，而一套没人保持得干净的告警集就不再有人看。它们是周期性审计的工具，
上面那条命令行就是复现这次审计的记录。

### 确认的缺陷：PKCS#11 对象句柄

`CK_OBJECT_HANDLE` 就是 `CK_ULONG`，而 `CK_ULONG` 是 `unsigned long`
（见 `third_party/pkcs11-v3.2/pkcs11t.h`），在 armv7l 上只有 4 字节。
而原来的句柄编码假设它有 8 字节：

```c
#define PUB_BIT (1ULL << 63)
return ((CK_OBJECT_HANDLE)m.generation << 32) | (CK_OBJECT_HANDLE)(slot + 1);
```

在目标板上这条移位是未定义行为（`-Wshift-count-overflow` 会报），
两个标志位在赋值给 4 字节句柄时被整个截掉。修复前在 QEMU 下复现出的
可观察后果：

- `pub == priv` —— 公钥与私钥对象退化成同一个句柄，于是
  `C_GetAttributeValue` 对公钥对象报的是 `CKO_PRIVATE_KEY`，
  并拒绝返回 `CKA_VALUE`；
- `C_SignInit` **接受了用公钥句柄签名**，而不是返回
  `CKR_KEY_TYPE_INCONSISTENT`，因为拒绝它的判据正是 `hKey & PUB_BIT`；
- `SECRET_BIT` 被截成 0，KEM 会话密钥对象因此完全取不到；
- generation 字段从所有句柄里消失，`C_DestroyObject` 之后旧句柄失效的机制
  不再起作用；
- 测试二进制最终段错误退出。

现在的编码与宽度无关，且整体落在 32 位内：bit 31 为公钥，bit 30 为会话密钥
对象，bits 12–29 是槽位 generation 的低 18 位，bits 0–11 是对象索引。
与 64 位核心句柄（`hsm_handle_t`）之间的转换是显式的，在
`p11_handle_of_core()` 与 `core_handle_of()` 里；核心层仍做完整的 64 位
generation 比较，所以把 generation 截到 18 位只影响本模块自己的前置检查。

### 确认的隐患：`off_t` 与 `time_t` 的默认宽度

在 armv7l 的 glibc 上，两者默认都是 32 位有符号。两个后果：

- 物理地址达到或超过 `0x8000_0000` 时，转成 `off_t` 会变成负数，
  `mmap` 以 `EINVAL` 失败。`accel_zynq.c` 目前映射的 PL 地址都在这条边界
  之下，但 `map_devmem()` 接受任意 `uint32_t`，而 PS 外设与 OCM 区段就在
  边界之上；
- 2038-01-19 之后 `time()` 返回负值，而审计与槽位元数据的时间戳会把它转成
  `uint64_t`，得到一个巨大的无意义数。

`CMakeLists.txt` 现在在 32 位目标上定义 `_FILE_OFFSET_BITS=64`，并在编译探测
确认 C 库支持（glibc 2.34 以上）之后定义 `_TIME_BITS=64`。带上这两个宏后实测
宽度：`off_t` 为 8，`time_t` 为 8。

需要注意 `ftell`/`fseek` 收发的是 `long`，无论如何都是 32 位。密钥库、备份与
锚点的读取用到它们，因此这几个文件被限制在 2 GiB 以内。它们都检查了偏短或
负数的返回并失败退出，所以这是一个有界的容量上限，不是正确性缺陷。

### 已改正：宽度敏感的解析

`uio_find_by_phys()` 原来用 `fscanf(f, "%lx", &addr)` 读 UIO 的映射地址，
`addr` 是 `unsigned long`。同一段 sysfs 文本在 `unsigned long` 为 4 字节
还是 8 字节时溢出行为不同。现在改为解析进 `unsigned long long`、用 `%llx`，
并在显式收窄后比较，于是开了 LPAE 的内核报出超过 4 GiB 的地址时只是匹配不上，
而不会匹配到一个被截断的值。

### 已改正：测试框架的收窄

`tests/testlib.h` 里的 `CHECK_EQ_INT` 原来经 `long` 比较，在目标板上会截断
64 位值：两个只在 bit 31 以上不同的值会被判为相等 —— 断言静默通过。
现在经 `long long` 比较。现有调用点都没有受影响（句柄比较用的是 `CHECK`
加原生类型），所以这是消除了一个潜在隐患，而不是修掉一个已发生的缺陷。

`tests/unit/test_p11.c` 原来把公钥标志位硬编码成 `1ULL << 63`，
现在改用与模块一致的、32 位安全的常量。

### 检查过并确认正确的部分

- **序列化。** 全代码没有任何整体结构体的 `memcpy`、`fwrite` 或 `pwrite`；
  所有落盘与在线格式都是逐字段、逐字节编码的。因此结构体填充
  （84 条 `-Wpadded`）只存在于内存布局里，不会进入任何格式。
  同一性质也意味着不存在字节序假设与非对齐宽访问：搜索转换到
  `uint16_t *`、`uint32_t *`、`uint64_t *` 等的强制转换，
  除 MMIO 寄存器窗口的 `volatile uint32_t *` 之外没有命中，
  而后者由 mmap 保证页对齐。
- **`CK_ULONG` 作为长度类型。** PKCS#11 的接口几乎所有长度都经 `CK_ULONG`
  传递，因此它收窄到 4 字节这件事与上面的句柄缺陷分开单独查过。
  `attr_ulong()` 的判据是 `a->ulValueLen == sizeof(CK_ULONG)` 而不是字面量 8，
  `fill_attr()` 与宽度无关，所有 `*pulXxxLen = (CK_ULONG)` 赋值的值都受
  ML-KEM / ML-DSA 的产物尺寸约束（最多几 KB）。模块与应用按同一 ABI 编译，
  所以跨边界共享的 `CK_ULONG` 宽度是一致的。
- **字符串转整数。** `cli/pqchsm_cli.c` 对 64 位的会话与对象句柄用
  `strtoull`，只对 `uint32_t` 字段用 `strtoul`，在两种宽度下都正确。
- **其余 `-Wconversion` 报告。** `tests/fuzz/fuzz_targets.c` 把
  `rnd() % n` 由 `uint64_t` 收窄到 `size_t`；结果受模数约束，不会丢值。
  `src/hal/pqc_accel.c` 把一个已做范围检查的正 `int` 转成 `size_t`。
  两处在任一宽度下都是正确的。
- **`%llu` 格式化。** 所有 64 位打印点本来就转成 `unsigned long long`
  并用 `%llu`，这正是与宽度无关的写法；没有任何一处用 `%lu` 打
  `size_t` 或 `uint64_t`。
- **`accel_zynq.c` 的寄存器与缓冲算术。** 偏移、长度与物理地址都显式为
  `uint32_t`，寄存器访问经 `volatile uint32_t *` 按字下标，缓冲边界在
  `uint32_t`/`size_t` 里检查 —— 这些在两种宽度下行为一致。
