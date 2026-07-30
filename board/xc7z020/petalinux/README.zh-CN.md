[English](README.md) · **中文**

# PetaLinux 路径

从 Vivado 构建产出的 `.xsa` 走到一张可启动的 SD 卡，加速器通过 UIO 暴露给用户态。
这是 XC7Z020 移植的产品形态；[PYNQ 路径](../pynq/README.zh-CN.md) 是上电调试形态。

> **下文所有 `petalinux-*` 命令都必须在装有 PetaLinux 的机器上执行，且没有一条
> 执行过。** 本仓库的开发机上既没有 PetaLinux，也没有 Vivado，也没有 Zynq 硬件。
> 本目录里的文件是照 PetaLinux 文档与 `create_project.tcl` 的地址映射写出来的；
> 第一个跑它的人应当预期需要对着手上那一版核对配置符号名与设备树标签。
> 容易需要核对的地方都在文件里就地标注了。

## 文件

| 文件 | 作用 | 放到哪里 |
|---|---|---|
| `system-user.dtsi` | 把加速器与 AXI-DMA 绑给 `generic-uio`；保留 DMA 缓冲 | `project-spec/meta-user/recipes-bsp/device-tree/files/` |
| `config-fragment` | 工程配置（bootargs）与内核配置（UIO、CMA） | 两个去处，见下 |
| `rootfs-config-fragment` | rootfs 运行时要有的包 | 追加到 `project-spec/configs/rootfs_config` |

## 前置条件

* **在装有 Vivado 的机器上**跑 `board/xc7z020/vivado/build_bitstream.tcl`，
  它在工程的 `outputs/` 下留下 `pqc_accel_bd_wrapper.xsa`。
* **在装有 PetaLinux 的机器上**：所用的 PetaLinux 版本，其 Yocto 层必须自带
  **OpenSSL 3**。顶层 `CMakeLists.txt` 写的是
  `find_package(OpenSSL 3.0 REQUIRED)`，1.1.1 不行。这一条要最先查，
  因为答案决定了这条路径是否走得通：

  ```sh
  bitbake -e openssl | grep "^PV="
  ```

  自带 1.1.1 时只有两条路：换更新的 PetaLinux，或者自己在 `meta-user` 下加一份
  OpenSSL 3 的 recipe。没有捷径。
* PetaLinux 与 Vivado 的版本要配套。`.xsa` 来自比 PetaLinux 更新的 Vivado 时，
  一般在 `--get-hw-description` 这一步就失败。

## 构建步骤

### 1. 建工程 —— 需在装有 PetaLinux 的机器上执行

```sh
petalinux-create -t project --template zynq --name pqc_zynq
cd pqc_zynq
```

`--template zynq` 是 Zynq-7000（Cortex-A9）的模板。`zynqMP` 是 UltraScale+ 的，
用错会做出一个 64 位镜像，在 XC7Z020 上根本起不来。

### 2. 导入硬件描述 —— 需在装有 PetaLinux 的机器上执行

```sh
petalinux-config --get-hw-description=<存放 .xsa 的目录>
```

参数是**目录**，不是文件。这一步从 `.xsa` 生成
`components/plnx_workspace/device-tree/device-tree/pl.dtsi` —— 里面就是
`system-user.dtsi` 要覆盖的 `pqc_accel_0` 与 `axi_dma_0` 两个节点。
菜单会弹出来；bootargs 在第 4 步由 fragment 替换，这里不用改什么，退出保存即可。

**先把生成的 `pl.dtsi` 看一遍**，再往下走：

```sh
grep -n "pqc_accel\|axi_dma" components/plnx_workspace/device-tree/device-tree/pl.dtsi
```

里面的标签必须与 `system-user.dtsi` 里 `&pqc_accel_0`、`&axi_dma_0` 两处覆盖对得上。
标签取自 block design 的实例名，正常情况下是对得上的；对不上会以
"undefined label" 让构建失败，而事先看过这个文件的话那条错误五秒就能改掉。
基址应当是 `0x43c00000` 与 `0x40400000`；不是的话，说明
`create_project.tcl` 与 `include/pqc_accel_zynq.h` 里的地址映射已经与硬件不一致，
要先解决那个问题。

### 3. 装设备树片段 —— 需在装有 PetaLinux 的机器上执行

```sh
cp <repo>/board/xc7z020/petalinux/system-user.dtsi \
   project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi
```

这个路径由 PetaLinux 的 device-tree 配方默认引入，不需要另写 `bbappend`。

**DMA 缓冲的地址要与板子实际的 DDR 容量核对。** 片段保留的是 `0x1F000000` 起的
64 KiB，与 `include/pqc_accel_zynq.h` 里的 `PQC_ZYNQ_DMA_BUF_PHYS` 对应。
这个地址落在 512 MiB DDR（`0x00000000`–`0x1FFFFFFF`）的最后 16 MiB 内，
而本移植的参考板 PYNQ-Z2 按板卡资料正是 512 MiB。`system-user.dtsi` 的注释里给了
一组现成的 1 GiB 取值（`0x3E000000`）供内存更大的板子使用；反过来替换是不安全的：
`0x3E000000` 在 512 MiB 的板子上根本不存在，内核会拒绝保留，或者保留出一段没有
背后存储的地址，写进去无声无息。上板前先确认容量：

```bash
head -1 /proc/meminfo
dmesg | grep -i memory
```

无论用哪一组，构建固件时都要把 `PQC_ZYNQ_DMA_BUF_PHYS` 定义成同一个值。
两处必须一致 —— 对不上时 DMA 会往一段不属于缓冲的物理内存搬数据。

### 4. 应用配置片段 —— 需在装有 PetaLinux 的机器上执行

`config-fragment` 里有两段，进两个不同的地方。按 marker 行切开：

```sh
FRAG=<repo>/board/xc7z020/petalinux/config-fragment

# 工程配置
awk '/^# ---- 工程配置/{f=1;next} /^# ---- 内核配置/{f=0} f' "$FRAG" \
    >> project-spec/configs/config
petalinux-config --silentconfig

# 内核配置
mkdir -p project-spec/meta-user/recipes-kernel/linux/linux-xlnx
awk '/^# ---- 内核配置/{f=1;next} f' "$FRAG" \
    > project-spec/meta-user/recipes-kernel/linux/linux-xlnx/pqc-uio.cfg
cat >> project-spec/meta-user/recipes-kernel/linux/linux-xlnx_%.bbappend <<'EOF'
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI += "file://pqc-uio.cfg"
EOF

# rootfs 配置
cat <repo>/board/xc7z020/petalinux/rootfs-config-fragment \
    >> project-spec/configs/rootfs_config
petalinux-config -c rootfs --silentconfig
```

这一步有两件事要知道：

* **`FILESEXTRAPATHS:prepend` 是新的 override 语法。** 底层 Yocto 在 honister
  之前的 PetaLinux 版本要写下划线形式 `FILESEXTRAPATHS_prepend`。bitbake 找不到
  `pqc-uio.cfg` 时先试这一条。
* **`config-fragment` 里的 bootargs 那行是模板，不是可以直接抄的值。** 它必须带上
  镜像实际使用的控制台与根设备。关掉 `CONFIG_SUBSYSTEM_BOOTARGS_AUTO=y` 之前先看
  它自动生成的是什么，以那一行为基础，只追加
  `uio_pdrv_genirq.of_id=generic-uio` 与 `cma=32M`。缺了前者，设备树节点写得再对，
  `/dev/uioN` 也不会出现。

内核选项要确认真的生效了 —— bitbake 没取到的 fragment 不会报错：

```sh
petalinux-config -c kernel      # 搜 UIO_PDRV_GENIRQ，期望是编进内核
```

### 5. 构建 —— 需在装有 PetaLinux 的机器上执行

```sh
petalinux-build
```

产物在 `images/linux/`：`zynq_fsbl.elf`、`u-boot.elf`、
（由 `.xsa` 带出的）`system.bit`、`image.ub`，以及 rootfs。

### 6. 打包启动镜像 —— 需在装有 PetaLinux 的机器上执行

```sh
petalinux-package --boot --fsbl images/linux/zynq_fsbl.elf \
                  --fpga images/linux/system.bit \
                  --u-boot --force
```

产出 `images/linux/BOOT.BIN`。`--fpga` 决定了比特流由 FSBL 在 Linux 启动之前加载。
不给这个参数的话，内核探测 UIO 节点时 PL 是空的：节点出现、`/dev/uioN` 出现、
每次读寄存器都是垃圾值 —— 正是 PYNQ README 故障分类里最让人困惑的那一种。

### 7. 做 SD 卡

两个分区：一个 FAT32 启动分区，一个 ext4 根分区。

| 分区 | 内容 |
|---|---|
| 1，FAT32 | `BOOT.BIN`、`image.ub` |
| 2，ext4 | rootfs，从 `images/linux/rootfs.tar.gz` 解开 |

设备名与分区大小取决于卡与宿主机，所以这一步没有写成脚本。
`config-fragment` 里的 `CONFIG_SUBSYSTEM_SDROOT_DEV` 写的是 `/dev/mmcblk0p2`，
要与卡实际的分区方式一致。

## 把固件放进 rootfs

固件在开发机上交叉编译，不由 bitbake 构建。它没有 Yocto recipe，加一份意味着
把构建方式维护两遍 —— CMake 那套是本项目唯一的构建描述。

**交叉编译**用仓库里已有的工具链文件：

```sh
cmake -S . -B build-armv7 \
      -DCMAKE_TOOLCHAIN_FILE=board/xc7z020/cmake/armv7-linux-gnueabihf.cmake \
      -DCMAKE_PREFIX_PATH=<liboqs 的交叉安装前缀>
cmake --build build-armv7
```

想用 PetaLinux 自带的工具链而不是发行版的 `arm-linux-gnueabihf-gcc`，
先生成 SDK：

```sh
petalinux-build --sdk                  # 需在装有 PetaLinux 的机器上执行
./images/linux/sdk.sh -d <sysroot 目录> # 装出 sysroot
```

然后构建时加 `-DPQCHSM_ARMV7_TRIPLE=arm-xilinx-linux-gnueabi`（实际前缀以装出来的
sysroot 的 `bin/` 里为准）与
`-DCMAKE_SYSROOT=<sysroot 目录>/sysroots/cortexa9*-xilinx-linux`。
用 PetaLinux 的 sysroot 才能保证二进制链接的是镜像自带的那一份 glibc 与 OpenSSL ——
发行版编出来的二进制可能在加载时以一句 `GLIBC_2.xx not found` 失败，
而那句话对真正的原因什么都没说。

**拷进去**有三种做法，按"能留多久"从短到长排：

1. **运行时 `scp`。** 需要 rootfs 里有 `openssh-sshd`（`rootfs-config-fragment`
   已经点了）且网络通。反复改的阶段最合适：不用重新构建，也不用拔卡。
2. **直接解到挂载好的 rootfs 分区里。** 启动前把可执行文件与 `liboqs.so*` 拷到卡的
   第二个分区。`liboqs` 没有 Yocto recipe，无论走哪种做法它的共享库都得跟着固件走；
   放进 `/usr/lib`，上板后跑一次 `ldconfig`。
3. **在 `meta-user` 下写一份安装预编译产物的 recipe。** 镜像要交付给别人的时候
   这才是正确答案，在那之前都属于过早。

无论哪种做法，运行时依赖都要在板上核实，不要假设：

```sh
ldd /usr/bin/<固件>              # 不应有 "not found"
openssl version                  # 期望 3.x
```

## 上板核对 UIO 绑定

第一次启动之后，跑任何东西之前先看这个：

```sh
ls /dev/uio*
for d in /sys/class/uio/uio*; do
    echo "$d $(cat $d/name) $(cat $d/maps/map0/addr) $(cat $d/maps/map0/size)"
done
```

应当出现两个设备，地址是 `0x43c00000` 与 `0x40400000`，各 64 KiB。
`accel_zynq.c` 按物理地址反查设备号而不是按名字匹配 —— 名字取决于设备树怎么写，
地址是硬件事实。

`/dev/uio*` 下什么都没有，说明 `uio_pdrv_genirq.of_id=generic-uio` 这个启动参数
没生效。按这个顺序查：

```sh
cat /proc/cmdline                        # 参数到底在不在
zcat /proc/config.gz | grep UIO          # 内核暴露了配置的话：应当是 =y 而不是 =m
dmesg | grep -i uio                      # probe 失败的记录，包括缺中断
```

probe 失败信息里提到中断的话，就是 `system-user.dtsi` 里警告的那种情况：某些内核
版本对没有 `interrupts` 属性的节点拒绝 probe `uio_pdrv_genirq`。本设计全程轮询、
不需要中断，所以正确的做法是把一个 PL 中断接到 `IRQ_F2P` 并补上这个属性，
而不是去改驱动。

另外确认没有别人抢走 DMA：

```sh
ls /sys/bus/platform/drivers/*/ | grep -i 40400000
```

`axi_dma_0` 必须绑在 `uio_pdrv_genirq` 上，不能绑在 `xilinx-vdma` 上。设备树里的
`compatible` 覆盖与 `config-fragment` 里的内核配置都是为了防这一条；内核驱动还是
抢到了的话，用户态的写会与它的状态机打架，而且不报错，只表现为传输偶发地不完成。
