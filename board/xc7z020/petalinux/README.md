**English** · [中文](README.zh-CN.md)

# PetaLinux path

From the `.xsa` produced by the Vivado build to a bootable SD card, with the
accelerator reachable from user space through UIO. This is the product form of the
XC7Z020 port; the [PYNQ path](../pynq/README.md) is the bring-up form.

> **Every `petalinux-*` command below must be run on a machine with PetaLinux
> installed, and none of them has been run.** This repository's development
> machine has neither PetaLinux nor Vivado nor Zynq hardware. The files in this
> directory are written against the PetaLinux documentation and the address map in
> `create_project.tcl`; the first person to run them should expect to correct
> config symbol names and device-tree labels against the version in front of them.
> Places where that is likely are flagged inline.

## Files

| File | Purpose | Where it goes |
|---|---|---|
| `system-user.dtsi` | Binds the accelerator and the AXI-DMA to `generic-uio`; reserves the DMA buffer | `project-spec/meta-user/recipes-bsp/device-tree/files/` |
| `config-fragment` | Project config (bootargs) and kernel config (UIO, CMA) | Two destinations, see below |
| `rootfs-config-fragment` | Packages the rootfs needs at runtime | Appended to `project-spec/configs/rootfs_config` |

## Prerequisites

* **On a machine with Vivado**: `board/xc7z020/vivado/build_bitstream.tcl`, which
  leaves `pqc_accel_bd_wrapper.xsa` in the project's `outputs/`.
* **On a machine with PetaLinux**: a PetaLinux release whose Yocto layer ships
  **OpenSSL 3**. The top-level `CMakeLists.txt` requires it
  (`find_package(OpenSSL 3.0 REQUIRED)`), and 1.1.1 will not do. Check before
  anything else, because the answer decides whether this path is viable at all:

  ```sh
  bitbake -e openssl | grep "^PV="
  ```

* The PetaLinux and Vivado versions should match. A `.xsa` from a newer Vivado than
  the PetaLinux release generally fails at `--get-hw-description`.

## Build steps

### 1. Create the project — PetaLinux machine

```sh
petalinux-create -t project --template zynq --name pqc_zynq
cd pqc_zynq
```

`--template zynq` is the Zynq-7000 (Cortex-A9) template. `zynqMP` is the
UltraScale+ one and will produce a 64-bit image that does not boot on an XC7Z020.

### 2. Import the hardware description — PetaLinux machine

```sh
petalinux-config --get-hw-description=<directory containing the .xsa>
```

The argument is the *directory*, not the file. This step generates
`components/plnx_workspace/device-tree/device-tree/pl.dtsi` from the `.xsa` —
the nodes for `pqc_accel_0` and `axi_dma_0` that `system-user.dtsi` overrides.
The menu opens; the fragment in step 4 replaces the bootargs, so nothing needs
changing here yet. Exit and save.

**Read the generated `pl.dtsi` now**, before going further:

```sh
grep -n "pqc_accel\|axi_dma" components/plnx_workspace/device-tree/device-tree/pl.dtsi
```

The labels there must match the `&pqc_accel_0` and `&axi_dma_0` overrides in
`system-user.dtsi`. They come from the block-design instance names, so they should
match, but a label mismatch fails the build with an "undefined label" error, and
knowing what the file says makes that error a five-second fix. The base addresses
should read `0x43c00000` and `0x40400000`; if they do not, the address map in
`create_project.tcl` and `include/pqc_accel_zynq.h` no longer agree with the
hardware and that has to be fixed first.

### 3. Install the device-tree fragment — PetaLinux machine

```sh
cp <repo>/board/xc7z020/petalinux/system-user.dtsi \
   project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi
```

That path is included by PetaLinux's device-tree recipe by default; no `bbappend`
is needed.

**Check the DMA buffer address against the board's actual DDR size.** The
fragment reserves 64 KiB at `0x1F000000`, matching `PQC_ZYNQ_DMA_BUF_PHYS` in
`include/pqc_accel_zynq.h`. That address is inside the last 16 MiB of a 512 MiB
DDR, which is what the reference board for this port, PYNQ-Z2, is documented as
having (`0x00000000`–`0x1FFFFFFF`). `system-user.dtsi` carries a ready-to-use
1 GiB alternative (`0x3E000000`) in a comment for boards with more memory; the
reverse substitution is not safe, because `0x3E000000` does not exist on a
512 MiB board — the kernel either refuses the reservation or reserves an address
range with no memory behind it, and writes to it vanish silently. Confirm the
size on the board:

```bash
head -1 /proc/meminfo
dmesg | grep -i memory
```

Whichever value you use, define `PQC_ZYNQ_DMA_BUF_PHYS` to the same one when
building the firmware. The two have to agree — a mismatch has the DMA writing to
physical memory that is not the buffer.

### 4. Apply the config fragments — PetaLinux machine

`config-fragment` holds two sections that go to two different places. Split it on
the marker lines:

```sh
FRAG=<repo>/board/xc7z020/petalinux/config-fragment

# project config
awk '/^# ---- 工程配置/{f=1;next} /^# ---- 内核配置/{f=0} f' "$FRAG" \
    >> project-spec/configs/config
petalinux-config --silentconfig

# kernel config
mkdir -p project-spec/meta-user/recipes-kernel/linux/linux-xlnx
awk '/^# ---- 内核配置/{f=1;next} f' "$FRAG" \
    > project-spec/meta-user/recipes-kernel/linux/linux-xlnx/pqc-uio.cfg
cat >> project-spec/meta-user/recipes-kernel/linux/linux-xlnx_%.bbappend <<'EOF'
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI += "file://pqc-uio.cfg"
EOF

# rootfs config
cat <repo>/board/xc7z020/petalinux/rootfs-config-fragment \
    >> project-spec/configs/rootfs_config
petalinux-config -c rootfs --silentconfig
```

Two things to know about this step:

* **`FILESEXTRAPATHS:prepend` is the newer override syntax.** PetaLinux releases
  built on Yocto before honister want `FILESEXTRAPATHS_prepend` with an
  underscore. If bitbake cannot find `pqc-uio.cfg`, that is the first thing to
  try.
* **The bootargs line in `config-fragment` is a template, not a value to copy.**
  It has to carry the console and root device your image actually uses. Look at
  what `CONFIG_SUBSYSTEM_BOOTARGS_AUTO=y` generated before turning it off, and
  keep that line, appending only `uio_pdrv_genirq.of_id=generic-uio` and `cma=32M`.
  Without the first of those, the device-tree nodes are correct and `/dev/uioN`
  still never appears.

Confirm the kernel options actually took effect, since a fragment that bitbake
never picked up fails silently:

```sh
petalinux-config -c kernel      # search for UIO_PDRV_GENIRQ, expect it built in
```

### 5. Build — PetaLinux machine

```sh
petalinux-build
```

Artefacts land in `images/linux/`: `zynq_fsbl.elf`, `u-boot.elf`,
`system.bit` (from the `.xsa`), `image.ub`, and the rootfs.

### 6. Package the boot image — PetaLinux machine

```sh
petalinux-package --boot --fsbl images/linux/zynq_fsbl.elf \
                  --fpga images/linux/system.bit \
                  --u-boot --force
```

This produces `images/linux/BOOT.BIN`. Passing `--fpga` is what gets the
bitstream loaded by the FSBL, before Linux starts. Without it the PL is empty
when the kernel probes the UIO nodes: the nodes appear, `/dev/uioN` appears, and
every register read returns garbage — one of the confusing failure modes
described in the PYNQ README's triage section.

### 7. Assemble the SD card

Two partitions: a FAT32 boot partition and an ext4 root partition.

| Partition | Contents |
|---|---|
| 1, FAT32 | `BOOT.BIN`, `image.ub` |
| 2, ext4 | the rootfs, unpacked from `images/linux/rootfs.tar.gz` |

The exact device names and partition sizes depend on the card and the host, so
this step is not scripted here. `CONFIG_SUBSYSTEM_SDROOT_DEV` in
`config-fragment` says `/dev/mmcblk0p2`, which has to match how the card is
actually partitioned.

## Getting the firmware into the rootfs

The firmware is cross-compiled on the development machine, not built by bitbake.
There is no Yocto recipe for it, and adding one would mean maintaining the build
twice — the CMake build is the single description of how this project is built.

**Cross-compile** with the toolchain file already in the repository:

```sh
cmake -S . -B build-armv7 \
      -DCMAKE_TOOLCHAIN_FILE=board/xc7z020/cmake/armv7-linux-gnueabihf.cmake \
      -DCMAKE_PREFIX_PATH=<liboqs cross-install prefix>
cmake --build build-armv7
```

To use PetaLinux's own toolchain instead of a distro `arm-linux-gnueabihf-gcc`,
generate the SDK and point the triple at it:

```sh
petalinux-build --sdk                  # PetaLinux machine
./images/linux/sdk.sh -d <sysroot dir> # installs the sysroot
```

then build with `-DPQCHSM_ARMV7_TRIPLE=arm-xilinx-linux-gnueabi` (check the
actual prefix in the installed sysroot's `bin/`) and
`-DCMAKE_SYSROOT=<sysroot dir>/sysroots/cortexa9*-xilinx-linux`. Using the
PetaLinux sysroot is what guarantees the binary links against the same glibc and
OpenSSL the image ships — a distro-built binary can fail at load time with a
`GLIBC_2.xx not found` that says nothing about the real cause.

**Copy in** — three options, in increasing order of permanence:

1. **`scp` at runtime.** Needs `openssh-sshd` in the rootfs
   (`rootfs-config-fragment` enables it) and networking up. Best while iterating:
   no rebuild, no card swap.
2. **Unpack into the mounted rootfs partition.** Copy the binaries and
   `liboqs.so*` onto the card's second partition before booting. `liboqs` has no
   Yocto recipe, so its shared library has to travel with the firmware either
   way; put it in `/usr/lib` and run `ldconfig` on the board.
3. **A `meta-user` recipe that installs prebuilt binaries.** The right answer once
   the image is something being handed to someone else, and premature before then.

Either way, verify the runtime dependencies on the board rather than assuming:

```sh
ldd /usr/bin/<firmware>          # no "not found" lines
openssl version                  # expect 3.x
```

## Checking the UIO binding on the board

After the first boot, before running anything:

```sh
ls /dev/uio*
for d in /sys/class/uio/uio*; do
    echo "$d $(cat $d/name) $(cat $d/maps/map0/addr) $(cat $d/maps/map0/size)"
done
```

Two devices should appear, at `0x43c00000` and `0x40400000`, each 64 KiB.
`accel_zynq.c` looks them up by physical address rather than by name, because the
name depends on how the device tree was written while the address is a hardware
fact.

Nothing at all in `/dev/uio*` means the `uio_pdrv_genirq.of_id=generic-uio`
bootarg did not take effect. Check in this order:

```sh
cat /proc/cmdline                        # is the parameter there at all
zcat /proc/config.gz | grep UIO          # if the kernel exposes it: =y, not =m
dmesg | grep -i uio                      # probe failures, including missing IRQ
```

A probe failure mentioning an interrupt is the case `system-user.dtsi` warns
about: some kernel versions refuse to probe `uio_pdrv_genirq` on a node with no
`interrupts` property. This design polls and needs no interrupt, so the fix is to
wire a PL interrupt to `IRQ_F2P` and add the property, not to change the driver.

Also confirm nothing else claimed the DMA:

```sh
ls /sys/bus/platform/drivers/*/ | grep -i 40400000
```

`axi_dma_0` must be bound to `uio_pdrv_genirq` and not to `xilinx-vdma`. Both the
device-tree `compatible` override and the kernel config in `config-fragment` exist
to prevent that; if the kernel driver wins anyway, user-space writes fight its
state machine, no error is reported, and transfers intermittently fail to
complete.
