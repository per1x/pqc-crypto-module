[English](USAGE.md) · **中文**

# 使用

> ### ⚠️ 被拒的访问读回 0，不报错
>
> 默认位流（`zu3eg_hsm.bit`）里，五个功能从机（槽 0、1、2、3、6）按 `SECURE_ONLY=1` 综合。
> Linux 发出的每一笔访问 `AxPROT[1]=1`，在总线上一律被拒，只有安全世界（EL3，经 BL31 的
> SiP）驱动得了它们。风扇从机（槽 5）没有防火墙。
>
> 拒绝走 RAZ/WI：读回 0，写被丢弃，响应是 OKAY，不产生总线错误。
>
> * 判断有没有被拒，看 `VERSION` 是不是 0。它在每个核上都是 `0x0001_0000`，读到 0 就是被拒
>   （或者位流没载入）。没有错误码可找。
> * 用户态怎么写都搞不崩板子。`hsm_nocrash` 在板上打进 36,000 笔被拒访问，板子照常运行。
>   早先的设计回 DECERR，AXI 的写是 posted 的，错误以 SError 回来，内核 panic，一个写错的地址
>   就是一次断电。
> * 打错地址不会有提示。违规计数器是记录，只有安全世界读得到。
>
> 板子现在启动的是开发位流 `zu3eg_hsm_dev.bit`（`PQC_DEV_OPEN=1`，功能从机 `SECURE_ONLY=0`），
> Linux 也可以经 `/dev/mem` 直接访问密码核。对称密钥装载口和种子口在两种位流里都只认安全事务。

- [仿真与静态检查](#仿真与静态检查)
- [主机软件](#主机软件)
- [位流](#位流)
- [服务层](#服务层)
- [运行 SDF 演示](#运行-sdf-演示)
- [PKCS#11 演示](#pkcs11-演示)
- [在板子上](#在板子上)
- [离线与内网安装](#离线与内网安装)

> ## ⛔ 先读这条：不可逆操作红线
>
> 这块板上禁止任何一次性、不可逆的烧写：eFUSE、eMMC 的 RPMB 认证密钥、BBRAM 锁存位、任何
> OTP 与 `*_LOCK` / `*_DISABLE` / `*_EN` 熔丝位。必须先得到板子所有者的明确同意；默认答案是
> 不，评估某件事也从来不等于允许执行它。
>
> 正式条文见
> [SECURITY.zh-CN.md「不可逆操作红线」](SECURITY.zh-CN.md#-不可逆操作红线)
> （熔丝清单、可逆的替代办法、2026-08-18 事故，以及这条规则为什么在编译期强制）。

## 仿真与静态检查

这些都不需要板子或有授权的工具链。

```bash
python3 -m venv .venv-rtl && ./.venv-rtl/bin/pip install cocotb
brew install icarus-verilog verilator yosys     # 或你发行版的对应包

./tools/rtl_sim_fast.sh     # 日常：按模块跑，Verilator
./tools/rtl_sim.sh          # 全套，Icarus 上 272 个 cocotb 用例，约 40 分钟
./tools/rtl_lint.sh         # Verilator -Wall + Icarus，hardware/rtl 下每个模块
./tools/rtl_synth_check.sh  # Yosys 可综合性
```

开发时用 `rtl_sim_fast.sh`（指定要跑的 testbench，或跑默认那组），合并前跑 `rtl_sim.sh`。
Verilator 是二值仿真，不传播 X，有些问题只有 Icarus 能暴露。

`rtl_lint.sh` 目前不通过：七个模块（`mldsa_axi`、`mldsa_engine`、`mldsa_keygen`、`mldsa_sign`、
`mldsa_verify`、`mlkem_axi`、`zu3eg_hsm_top`）仍有 Verilator 警告，主要是常量位宽扩展和未使用
信号。其余模块是干净的。

`rtl_sim.sh` 首次运行时生成黄金向量（`python3 hardware/model/export_vectors.py`）。缺工具时各脚本
会打印信息后跳过，不会失败。

## 主机软件

需要 CMake ≥ 3.20、C11 编译器、OpenSSL 3。后量子算法来自自带的 mlkem-native / mldsa-native
（`third_party/pqc-native/`，OP-TEE TA 也用它）；不需要 liboqs。

```bash
brew install openssl@3 cmake               # macOS
# Debian/Ubuntu：libssl-dev、cmake、ninja-build
```

```bash
./tools/fetch_vectors.sh                   # NIST ACVP 向量（固定 commit）
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

可选组件是自动探测的，不是必需。没有 Verilator 时仿真 RTL 后端不编进去，
`accel_transport_verilator()` 返回 `NULL`；需要 `cocotb`、`iverilog` 或 `pkcs11-tool` 的测试会
打印信息后自行跳过。

其他检查：

```bash
python3 tools/ct_audit.py       # 恒定时间源码审计（先跑 --self-test）
python3 tools/check_zeroize.py  # 清零结构检查（先跑 --self-test）
./tools/aarch64_test.sh         # 在 aarch64 容器里完整重建并回归
./tools/fuzz.sh                 # libFuzzer 目标（需要 LLVM clang）
./tools/profile.sh              # 采样剖析
./build/pqchsm-bench            # 算法级基线
./build/pqchsm-prim-bench       # 逐原语开销与实测 RTL 周期数
```

## 位流

Vivado 2020.1，目标 `xazu3eg-sfvc784-1-i`，约 35 分钟。

```bash
vivado -mode batch -source hardware/syn/impl_bitstream.tcl
# → hardware/syn/impl/zu3eg_hsm.bit       默认：功能从机 SECURE_ONLY=1

PQC_DEV_OPEN=1 vivado -mode batch -source hardware/syn/impl_bitstream.tcl
# → hardware/syn/impl/zu3eg_hsm_dev.bit   功能从机 SECURE_ONLY=0
```

`boot/d7/bitbuild-d7.sh` 在构建机上同时出这两份。板子启动的是开发位流；默认位流手动载入，用于
核对边界。

特性化构建会降低风扇门限并引出原始 TRNG 抽头。它不是产品形态：产品构建里这个抽头在逻辑阵列
中根本不存在。

```bash
PQC_CHARACTERIZE=1 vivado -mode batch -source hardware/syn/impl_bitstream.tcl
# → hardware/syn/impl/zu3eg_hsm_char.bit
```

流程里任何一条综合后断言不过就会中止：被驱动的 PS 握手信号、风扇引脚、`SYSMONE4 SIM_DEVICE`、
负的建立时间裕量，以及低于 0.050 ns 的有效保持裕量。每一条的说明见
[TESTING.zh-CN.md](TESTING.zh-CN.md#实现流程断言)。

单个模块的 out-of-context 综合，看面积和 Fmax：

```bash
vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs mlkem_decaps
```

## 服务层

`service/` 构建 daemon、SDF 风格客户端库、演示程序和工具：

```bash
make -C service                              # 主机构建
make -C service CROSS=aarch64-linux-gnu- SYSROOT=<目标 sysroot> SSLROOT=<交叉编译的 openssl>   # 板子用
```

目标有：`pqchsm_fpgad`（会话、句柄、串行化）、`libsdfe.a`（客户端库）、`sdf_demo`、`hsmctl`、
`hsm_httpd`。板上的程序动态链接 OpenSSL，需要 `/media/sd-mmcblk1p2/hsm/lib` 里有
`libssl.so.1.1` 和 `libcrypto.so.1.1`，细节见 [service/README.md](../service/README.md)。
构建机上的交叉编译由 `boot/d7/build-apps-d7.sh` 完成。

daemon 需要 `/dev/secmmio`，由 `board/kmod/` 里的内核模块提供，而它又需要带有 `boot/atf/` 里
SiP 的 BL31。密钥操作还需要装好 TA（`tee/`）并运行 `tee-supplicant`，`hsm-boot.sh` 会处理这两件事。
SiP 服务和白名单的说明见 [boot/README.md](../boot/README.md)。

## 运行 SDF 演示

在板子上，daemon 已运行（`READY=yes`，见[在板子上](#在板子上)）：

```bash
./sdf_demo                                # 本机
```

### 功能巡览（不需要板子）

不用任何准备就能看到主机侧模块的管理功能（算法支持、槽位、密钥、M-of-N 备份、安全存储）：

```bash
./demo/functions/run.sh
```

它自带一个使用临时 keystore 的 daemon，退出时一并删除。这里演示的是主机侧模块 `pqchsmd`，不是
板上的 daemon，两者的差别列在 [demo/functions/README.zh-CN.md](../demo/functions/README.zh-CN.md)。

### 对着板子一条命令

板子如果已经配好，任何能连到它的机器都不用配置就能跑完整演示：

```bash
./demo/remote/run.sh                     # 先问板子地址，再问跑什么
./demo/remote/run.sh <board-ip> --smoke  # 也可以直接给地址；--status 只测连通
```

不带参数时它会交互提问（连通后还会提议记住地址）。从管道或 CI 里调用时，它打印用法后退出，
不会阻塞。

这条路径使用提交在 `demo/remote/creds/` 下的公开演示凭据。除了局域网演示之外，用它做别的事
之前先读 [demo/remote/README.zh-CN.md](../demo/remote/README.zh-CN.md) 里的代价说明。

### 配置脚本（板子用自己的私有 PKI）

```bash
./tools/demo_remote.sh <board-ip> --provision   # 一次：生成 PKI、装到板子、留下客户端凭据
./tools/demo_remote.sh <board-ip>              # 之后纯本地：构建 + mTLS 调用，不用 SSH
./tools/demo_remote.sh <board-ip> --smoke      # 最小冒烟，只打关键行
./tools/demo_remote.sh <board-ip> --status     # 只确认设备有应答
```

任何密码运算都不经过 SSH。客户端是本地原生程序，直接连板子的 TCP 9797 端口。只有 `--provision`
用 SSH：它生成设备 PKI，把设备证书装到板子上，在本地保留客户端证书
（`~/.config/pqchsm/pki`，权限 0600），并校准板子的时钟。

远程口用 mTLS，没有 token 这条路。设计和局限见 `service/pqcs_tls.h` 和
[SECURITY.zh-CN.md](SECURITY.zh-CN.md) 的「远程口」一节。

真实部署里，客户端证书应当带外分发（或者由运维生成 CSR 再签发）。`--provision` 只是方便手上
有板子 shell 的人。CA 私钥始终留在你的机器上。

较大的时钟偏差会让握手失败。证书带 notBefore/notAfter，而板子重启后时钟可能落在它们之前。TLS 1.3
的拒绝还是在客户端认为握手已完成之后才到达，所以症状是「设备信息打印出乱码」，不是「认证失败」。
`--provision` 会校准时钟，`hsm-boot.sh` 会在时钟比 SD 卡上的凭据还早时把它往前拨，客户端在打开
设备时还会做一次 OP_PING 往返来暴露这类拒绝。重启后 mTLS 报证书错误，先查时钟。

可覆盖的变量：`BOARD=… PORT=… PQCHSM_PKI=… DEVICE_CN=… SSH_KEY=…`。每种失败都会打印下一步该做什么。

### 手工做同样的事

本地和远程两种形式是同一个程序，只有打开设备的那一行不同。它链接 `libsdfe` 和 OpenSSL，OpenSSL
只用于 TLS 传输，里面没有 ML-KEM、ML-DSA、SM4 或 SM3 的实现。`make -C service` 会构建它，下面是
等价的手工命令。

```bash
cc -O2 -Iservice -o sdf_demo \
   service/sdf_demo.c service/libsdfe.c service/pqcs_tls.c -lssl -lcrypto
# macOS 自带的是 LibreSSL，没有 TLS 1.3 API；用 Homebrew 的 openssl@3：
#   P=$(brew --prefix openssl@3); cc -O2 -I$P/include -Iservice ... -L$P/lib -lssl -lcrypto
```

```bash
# 凭据目录里要有 hsm_ca.crt / client.crt / client.key
./sdf_demo 192.168.50.175 ~/.config/pqchsm/pki 9797 axu3egb-hsm-01
#          └ 板子 IP      └ 凭据目录           └ 端口 └ 期望的设备 CN（可选）
```

远程板子地址是 PS 侧的 `eth1`。这块板子（dropbear 2019.78）用新版 OpenSSH 连接需要
`-o PubkeyAcceptedAlgorithms=+ssh-rsa`，这只影响 `--provision`；不加的话会得到
`Permission denied (publickey,password)`，尽管公钥已经装好。

只有 `hsm_ca.crt`、`hsm_device.crt`、`hsm_device.key` 三个文件都在板子的 `pki/` 目录里时，daemon
才会监听远程口。

`sdf_demo` 里没有任何密码算法，所以它打印的每个正确结果都来自板子。各步骤：

| 步骤 | 调用 | 说明 |
|---|---|---|
| 1 | `SDFE_GenerateRandom` | 从 PL TRNG 取 32 字节 |
| 2 | `SDFE_GenerateKeyPair_MLKEM`（768） | 返回公钥和句柄。应用从不拿到 `dk`，种子由 TA 生成并保管 |
| 3 | `SDFE_Encapsulate_MLKEM` | 用公钥得到共享秘密和密文 |
| 4 | `SDFE_Decapsulate_MLKEM` | 按句柄解封装，得到相同的共享秘密；PL 为这次调用展开私钥并清零 |
| 5 | `SDFE_ImportKey`、`SDFE_LoadKatKey`、`SDFE_GenerateKey`、`SDFE_LoadKey`、`SDFE_Encrypt` | 明文导入被拒（`SDR_NOTSUPPORT`）。公开的 GB/T 32907 SM4 向量装入 KAT 槽 7 并核对。TA 生成 SM4 密钥进槽 3 并交回 blob；把 blob 重装进槽 4 得到相同密文；blob 装不进槽 7 |
| 6 | `SDFE_GenerateKeyPair_MLDSA`、`Sign`、`Verify` | ML-DSA 44/65/87 全流程，改一个字节后必须验签失败 |

用 `hsmctl` 和 `hsm_httpd` 的业务流程见 [README.zh-CN.md](../README.zh-CN.md)。

## PKCS#11 演示

```bash
cmake --build build --target pqchsm-p11

# Python，经 PyKCS11
python3 -m venv .venv-p11 && ./.venv-p11/bin/pip install -q PyKCS11
./.venv-p11/bin/python demo/python/pqchsm_demo.py

# Java，经 JDK 22+ 的 FFM API（无外部依赖）
java --enable-native-access=ALL-UNNAMED demo/java/PqcHsmDemo.java \
     "$PWD/build/pqchsm-pkcs11.dylib"
```

两者都走完整生命周期：初始化 token、设置 PIN、登录、在槽位里生成 ML-DSA 和 ML-KEM 密钥、签名与
验签、读取属性、枚举对象。它们用的是主机侧模块的软件后端，不需要板子。两者都直接用底层绑定，
因为更高层的 provider 框架还不支持这些机制，见 [demo/README.zh-CN.md](../demo/README.zh-CN.md)，
里面有一个说明这一点的探针。

## 在板子上

`board/scripts/hsm-boot.sh` 每次开机从 SD 卡（`/media/sd-mmcblk1p2/hsm/`）运行。它校正时钟，
配置 `eth1`（`192.168.50.175`），载入 `zu3eg_hsm_dev.bit`，加载 `secmmio.ko`，装 TA 并启动
`tee-supplicant`，然后启动 `pqchsm_fpgad` 和 `hsm_httpd 8443 8080`。全部成功时
`hsm/HSM_STATUS` 以 `READY=yes` 结尾，上电后约 35 秒。步骤和其他脚本列在
[board/README.md](../board/README.md)。运行时切换到默认位流：

```sh
sh /media/sd-mmcblk1p2/hsm/pl_deadman.sh /media/sd-mmcblk1p2/hsm/zu3eg_hsm.bit 300
```

宽限期内如果没有人通过 `eth1` 登录并 `touch hsm/PL_KEEP`，它会回滚。

凡是要重配 PL 的操作都必须经过 harness：

```sh
sh /media/sd-mmcblk1p2/hsm/plharness.sh <payload.sh>
```

`board/scripts/plharness.sh` 会脱离终端，退出时恢复网络，并启动 sysrq 看门狗。

- `eth0` 在 PL 里。`80000000.ethernet` 是厂家设计里的 AXI Ethernet，所以每次重配前都要先解绑
  PL 驱动，之后再重新绑定。带着活的 AXI 主口重配会把总线挂死，之后连 sysrq 都写不进去，只能
  断电恢复。
- 不要把解绑命令放在前台 SSH 命令里。第一个被解绑的就是 `eth0`，会话当场断开，后面的命令都
  不会执行。
- 收尾阶段不信任 payload 留下的状态。有的 payload 会自己把驱动绑回去，所以重配前无条件先解绑
  一遍；对已经解绑的设备再解绑只是一条被忽略的错误。

payload 脚本在 `board/scripts/`（`pay_*.sh`），它们运行的程序在 `board/src/`。每次上板运行的原始
输出都保留在 `board/logs/`。

风扇控制器是密码位流的一部分（`fan_ctrl`，AXI 观测口在槽 5），位流载入后由它控制风扇。在此之前
板子运行厂家位流；`board/factory/` 重建了一份替换掉风扇逻辑的版本，PetaLinux 的 `fanquiet` 配方
在开机时载入带温控的位流。见 [board/README.md](../board/README.md)。

## 离线与内网安装

把依赖逐项搬到无法访问互联网的主机上（ACVP 向量、PyKCS11、JDK，以及连 `apt`/`dnf` 都不可用
时的备选办法）的步骤见 [reference/deployment.zh-CN.md](reference/deployment.zh-CN.md)。
