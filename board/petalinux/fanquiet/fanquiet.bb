SUMMARY = "Load the temperature-controlled PL bitstream early at boot (quiet fan)"
DESCRIPTION = "\
出厂 bitstream 把风扇脚 AA11 钉死在低电平＝常转满速。这个 recipe 把一份\
「厂家设计 + 风扇温控」的 bitstream 连同加载脚本一起装进 initramfs，\
开机早期就换上去。\
\
为什么走 Linux init 而不是 boot.scr 里的 fpga loadb：那条路试过，U-Boot 挂死，\
板子起不来要人拔 SD 卡 —— 而黄金 BOOT.BIN 的 FSBL 不武装看门狗、板子也没有\
JTAG，boot.scr 一旦挂住没有任何兜底。走 image.ub 这条路则落在现成的\
三振退备份镜像机制里（boot_try1/2/3 -> use_backup），坏了第三次启动自动退回，\
不需要人插手。\
\
bitstream 烤进 initramfs 而不是放 SD 卡上，是为了去掉对挂载顺序的依赖：\
脚本跑在 S20（网络初始化之前），那时 /media/sd-* 未必已经挂上。"

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://fanquiet.init file://fanquiet.bit"

inherit update-rc.d
INITSCRIPT_NAME = "fanquiet"
# start 20：**在网络初始化（S40）之前**。这样重配 PL、重绑驱动都发生在
# 网络起来之前，后面的 networking 脚本照常配 IP —— 脚本自己一个字节的
# 网络配置都不用碰。
# 也必须在 bootok（S99，延迟 60 秒清三振标记）之前很久 —— 万一这一步把板子
# 搞挂，标记还在，第三次启动会退回备份镜像。
INITSCRIPT_PARAMS = "start 20 3 5 ."

do_install() {
    install -d ${D}${sysconfdir}/init.d
    install -m 0755 ${WORKDIR}/fanquiet.init ${D}${sysconfdir}/init.d/fanquiet
    install -d ${D}${nonarch_base_libdir}/firmware
    install -m 0644 ${WORKDIR}/fanquiet.bit ${D}${nonarch_base_libdir}/firmware/fanquiet.bit
}

FILES_${PN} = "${sysconfdir}/init.d/fanquiet ${nonarch_base_libdir}/firmware/fanquiet.bit"
