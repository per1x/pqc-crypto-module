[English](USAGE.md) · **中文**

# 使用

> ### ⚠️ 写任何寄存器之前先看这一段
>
> **默认位流（`zu3eg_hsm.bit`）对普通世界是零可达的。** 四个功能从机都按
> `SECURE_ONLY=1` 综合，而 Linux 发出的每一笔事务 `AxPROT[1]` 恒为 1，
> 因此在总线上一律被拒。只有安全世界（EL3，经 BL31 的 SiP）驱动得了它们。
>
> 被拒的访问走 **RAZ/WI**：**读回 0、写被丢弃，响应是 OKAY，不产生任何总线
> 错误**。所以：
>
> * **判断被没被拒，看 `VERSION` 是不是 0。** 它在每个核上都是 `0x0001_0000`，
>   读到 0 就是被拒了（或者位流没载）。别去找错误码 —— 没有。
> * **用户态怎么写都搞不崩板子。** 这是有意为之，仿真里已经确立；真硅上的那道
>   核对也做了：`hsm_nocrash` 打进 36,000 笔被拒访问，板子照常活着。
>   早先的版本回 DECERR，
>   而 AXI 的写是 posted 的，错误以 **SError** 回来，
>   内核只能 panic —— 一个写错的地址就是一次断电。这条已经没有了。
> * **打错地址现在是安静的。** 补偿是违规计数器，只有安全世界读得到。
>
> `PQC_DEV_OPEN=1` 仍然会出 `SECURE_ONLY=0` 的 `zu3eg_hsm_dev.bit`，让普通世界
> 能直接驱动密码核做调试 —— 但它不再是"为了能安全地写"而必需的东西。


- [仿真与静态检查](#仿真与静态检查)
- [主机软件](#主机软件)
- [Bitstream](#bitstream)
- [服务层](#服务层)
- [运行 SDF 演示](#运行-sdf-演示)
- [PKCS#11 演示](#pkcs11-演示)
- [在板子上](#在板子上)
- [离线与内网安装](#离线与内网安装)

## 仿真与静态检查

本节内容都不需要板子，也不需要带 license 的工具链。

```bash
python3 -m venv .venv-rtl && ./.venv-rtl/bin/pip install cocotb
brew install icarus-verilog verilator yosys     # or your distro's packages

./tools/rtl_sim.sh          # 197 cocotb tests
./tools/rtl_lint.sh         # Verilator -Wall + Icarus, 70 modules
./tools/rtl_synth_check.sh  # Yosys synthesisability, 68 modules
```

`rtl_sim.sh` 首次运行时会生成黄金向量
（`python3 hardware/model/export_vectors.py`）。工具缺失时，每个脚本都会打一条
消息跳过，而不是失败。

## 主机软件

依赖：CMake ≥ 3.20、C11 编译器、OpenSSL 3、liboqs。

```bash
brew install liboqs openssl@3 cmake        # macOS
# Debian/Ubuntu: libssl-dev, cmake, ninja-build; liboqs from source
```

```bash
./tools/fetch_vectors.sh                   # NIST ACVP vectors (pinned commit)
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

可选组件是探测出来的，不是必需的。没有 Verilator 时，仿真 RTL 后端不会被编进来，
`accel_transport_verilator()` 返回 `NULL`；需要 `cocotb`、`iverilog` 或
`pkcs11-tool` 的测试会打一条消息自行跳过，而不是失败。

其他检查：

```bash
python3 tools/ct_audit.py       # constant-time source audit (--self-test first)
python3 tools/check_zeroize.py  # zeroization structure check (--self-test first)
./tools/aarch64_test.sh         # full rebuild and regression in an aarch64 container
./tools/fuzz.sh                 # libFuzzer targets (needs LLVM clang)
./tools/profile.sh              # sampling profile
./build/pqchsm-bench            # algorithm-level baseline
./build/pqchsm-prim-bench       # per-primitive cost and measured RTL cycle counts
```

## Bitstream

Vivado 2020.1，目标器件 `xazu3eg-sfvc784-1-i`，约 35 分钟。

```bash
vivado -mode batch -source hardware/syn/impl_bitstream.tcl
# → hardware/syn/impl/zu3eg_hsm.bit
```

表征构建会降低风扇阈值，并把 TRNG 的原始抽头暴露出来。**这不是产品形态**——在
产品构建里，那个抽头在逻辑中根本不存在，而不是读出来是零：

```bash
PQC_CHARACTERIZE=1 vivado -mode batch -source hardware/syn/impl_bitstream.tcl
# → hardware/syn/impl/zu3eg_hsm_char.bit
```

综合后的任何一条断言不过，流程即中止——被驱动的 PS 握手信号、风扇引脚、
`SYSMONE4 SIM_DEVICE`、setup 裕量为负，以及有效保持裕量低于 0.050 ns。每一条都
记在 [TESTING.zh-CN.md](TESTING.zh-CN.md) 里，连同它是花什么代价换来的。

对单个模块做 out-of-context 综合，用于看面积与 Fmax：

```bash
vivado -mode batch -source hardware/syn/ooc_synth.tcl -tclargs mlkem_decaps
```

## 服务层

`service/` 构建守护进程、SDF 风格客户端库与演示程序：

```bash
make -C service                              # host build
make -C service CROSS=aarch64-linux-gnu-     # for the board
```

三个产物：`pqchsm_fpgad`（会话、句柄、串行化）、`libsdfe.a`（客户端库）与
`sdf_demo`。它们是静态链接的，因此可以直接拷到板子上，不用操心板上的 libc。

守护进程需要 `/dev/secmmio`，它由 `board/kmod/` 里的内核模块提供，而该模块又需要
一个带上 `boot/atf/` 中那个 SiP 的 BL31。

## 运行 SDF 演示

### 在板子上（本机）

```bash
./service/pqchsm_fpgad &
./service/sdf_demo
```

### 从 Mac / 任意主机远程（**推荐,不用登板子**）

`sdf_demo` 的本机版和远程版是**同一个程序**，只有开设备那一行不同。
它只链接 `libsdfe`（纯 socket，**不依赖 OpenSSL / liboqs**），所以两个文件就能编：

```bash
cc -O2 -Iservice -o sdf_demo service/sdf_demo.c service/libsdfe.c
```

⚠️ 仓库里**没有** `sdf_demo` 的 CMake 目标（上面那行就是它唯一的编译方式）。
本节以前写的 `./service/sdf_demo` 是手工编出来的产物，容易让人以为 `cmake --build`
会生成它。

```bash
# 取一次性口令（板子的 dropbear 是 2019.78，需要这两个 -o；见下）
TOK=$(ssh -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
         root@192.168.50.175 'cat /media/sd-mmcblk1p2/hsm/hsm_token')

./sdf_demo 192.168.50.175 "$TOK"        # 端口默认 9797，可加第三个参数改
```

⚠️ **现代 OpenSSH 连这块板要两个 `-o`**：`HostKeyAlgorithms=+ssh-rsa` 让它接受板子的
RSA 主机密钥，`PubkeyAcceptedAlgorithms=+ssh-rsa` 让它愿意用 RSA(SHA-1) 签名认证。
少了后者会得到 `Permission denied (publickey,password)` —— **不是公钥没装**，
而是本机 OpenSSH 默认不再发这种签名。

⚠️ **没有 `hsm_token` 文件 daemon 就不监听**（fail-closed）。远程口是 TCP **9797**。

`sdf_demo` **只**链接 `libsdfe`——完全不链接任何密码库——所以它自己算不出任何
东西。它打印出的每一个正确数值都出自 FPGA。

```
[device] pqchsm_fpgad on FPGA  mlkem=0x00010000 sym=0x00010000

[1] SDFE_GenerateRandom             32 bytes from the PL ring-oscillator source
[2] SDFE_GenerateKeyPair_MLKEM(768) ek 1184 bytes, private key handle = 0
                                    ← the application never receives dk
[3] SDFE_Encapsulate_MLKEM          K 32 + c 1088 bytes
[4] SDFE_Decapsulate_MLKEM          K recovered by handle matches [3]
[5] SDFE_ImportKey → key_vault slot 3; SDFE_Encrypt(SM4)
    ciphertext 681edf34d206965e86b3e94f536e4246
    byte-exact against GB/T 32907 A.1; decryption returns the plaintext
```

同一次运行中，一个反证程序从普通世界直接读这五个核，得到 **6/6 被拒**。
（测于 RAZ/WI 改动之前，所以日志里记的是 DECERR；在当前位流上同样这几笔会读回 0。）应用
能用；绕过服务层去碰硬件不能用。

## PKCS#11 演示

```bash
cmake --build build --target pqchsm-p11

# Python, via PyKCS11
python3 -m venv .venv-p11 && ./.venv-p11/bin/pip install -q PyKCS11
./.venv-p11/bin/python demo/python/pqchsm_demo.py

# Java, via the JDK 22+ FFM API (no external dependency)
java --enable-native-access=ALL-UNNAMED demo/java/PqcHsmDemo.java \
     "$PWD/build/pqchsm-pkcs11.dylib"
```

两者都跑完整生命周期：初始化令牌、设置 PIN、登录、在槽位里生成 ML-DSA 与 ML-KEM
密钥、签名与验签、读属性、枚举对象。两者都直接使用低层绑定，因为上层 provider
框架尚不支持这些机制——见 [demo/README.zh-CN.md](../demo/README.zh-CN.md)，里面有
一个探测程序把这一点演示出来。

## 在板子上

**任何碰到 PL 的操作都必须经过 harness。**

```sh
sh /media/sd-mmcblk1p2/hsm/plharness.sh <payload.sh>
```

`board/scripts/plharness.sh` 会从终端脱离，退出时无条件恢复网络，并挂上一个
sysrq 看门狗。这不是走过场：

- **`eth0` 在 PL 里。** `80000000.ethernet` 是厂家设计里的 AXI Ethernet，所以每次
  重配 PL 都必须先解绑 PL 驱动、事后再绑回去。带着活的 AXI 主口重配逻辑会挂住
  总线——而一旦 AXI 倒了，连 sysrq 都写不进去，看门狗同样救不回来。只有断电重启
  能救。
- **绝不要把解绑放进前台的 SSH 命令里。** 第一个被解绑的设备就是 `eth0`；会话
  当场断掉，它后面的一切再也不会执行。三次断连、三次断电重启就是这么来的。
- **收尾流程不信任 payload 留下的状态。** 它在重配前无条件解绑，因为有些 payload
  自己会把驱动绑回去。解绑一个已经解绑的设备只是一条被忽略的报错；另一个选项是
  挂死的总线。

payload 脚本在 `board/scripts/`（`pay_*.sh`）；它们运行的程序在 `board/src/`。
每次板上运行的原始输出都原样保存在 `board/logs/`。

风扇控制完全可用，但还不能跨重启保持——一条命令就能让板子安静下来：

```sh
sh /media/sd-mmcblk1p2/hsm/fanquiet-init.sh
```

要让它熬过重启，就得写黄金 `BOOT.BIN` 的 PL 部分，而那需要有 JTAG 才可挽救。见
[SECURITY.zh-CN.md](SECURITY.zh-CN.md)。

## 离线与内网安装

把所有依赖搬到一台没有外网路由的机器上——liboqs、ACVP 向量、PyKCS11、JDK，以及
连 `apt`/`dnf` 都用不了时的兜底方案——在
[reference/deployment.zh-CN.md](reference/deployment.zh-CN.md) 里有逐步说明。
