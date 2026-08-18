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


> ## ⛔ 先读这条：不可逆操作红线
>
> **这块板上禁止任何一次性、不可逆的烧写** —— eFUSE、eMMC 的 RPMB 认证密钥、
> BBRAM 锁存位、任何 OTP 与 `*_LOCK` / `*_DISABLE` / `*_EN` 熔丝位。
> 必须先取得板子所有者的明确同意，**默认一律"否"**；"评估过"不等于"可以执行"。
>
> **完整规则以 [SECURITY.zh-CN.md — 不可逆操作红线](SECURITY.zh-CN.md#-不可逆操作红线)
> 为准**（熔丝清单、可逆替代、2026-08-18 事故经过，以及它为什么是编译期拦的）。

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

### 一键脚本（**最省事，先用这个**）

```bash
./tools/demo_remote.sh --provision   # 只需做一次：生成 PKI、装板子、留凭据
./tools/demo_remote.sh               # 之后纯本地：编译 + mTLS 调用，零 SSH
./tools/demo_remote.sh --smoke       # 最小冒烟，只打关键结论
./tools/demo_remote.sh --status      # 只连一下看设备在不在
```

**密码运算一步都不经过 SSH。** 客户端是本机原生二进制，自己开 TCP 到板子的
9797 口。`--provision` 是唯一会用 SSH 的动作，它做四件事：生成一套设备 PKI、
把设备证书装到板上、在本机留一份客户端凭据、给板子对时。装完之后
（凭据在 `~/.config/pqchsm/pki`，0600）就不再需要板子的 shell。

⚠️ **远程口是 mTLS，不再是明文口令**（2026-08-18 改）。以前那条
`--fetch-token` 的路已经删掉 —— 它是明文 TCP + 一条静态预共享口令：
同网段抓一次包就永久接管，整条会话既不保密也不防篡改，认证帧还能重放。
换掉的理由与新设计写在 `service/pqcs_tls.h` 的文件头。

⚠️ 真实部署里客户端证书应当**带外分发**（或由操作端自己出 CSR 送签），
`--provision` 只是给手上就有板子 shell 的人省事。**CA 私钥只留在本机**，
一步都不上板。

⚠️ **两边时钟差太多会握不上手**：证书有 notBefore/notAfter，而这块板的时钟
掉过就会落到证书生效之前。更坑的是 TLS 1.3 的拒绝是**握手之后**才送到的，
症状表现为"设备信息是一串乱码"而不是"认证失败"。所以 `--provision` 里有一步
对时（并写 RTC），`hsm-boot.sh` 开机也会做一次"时钟不许比 SD 上的凭据还早"
的兜底。客户端那侧则在开设备时多走一个 OP_PING 来回，把这类拒绝逼出来。

覆盖用环境变量：`BOARD=… PORT=… PQCHSM_PKI=… DEVICE_CN=… SSH_KEY=…`。
**每一步失败都会给出可执行的下一步**，而不是只回一个非零退出码。

### 手工做同样的事（想知道每一步在干什么就看这里）

`sdf_demo` 的本机版和远程版是**同一个程序**，只有开设备那一行不同。
它链接 `libsdfe` 与 OpenSSL（**OpenSSL 只做 TLS 传输**，里面没有 ML-KEM /
ML-DSA / SM4 / SM3 的任何实现），三个文件就能编：

```bash
cc -O2 -Iservice -o sdf_demo \
   service/sdf_demo.c service/libsdfe.c service/pqcs_tls.c -lssl -lcrypto
# macOS：系统自带的是 LibreSSL，缺 TLS 1.3 的 API，要用 Homebrew 那份
#   P=$(brew --prefix openssl@3); cc -O2 -I$P/include -Iservice ... -L$P/lib -lssl -lcrypto
```

⚠️ 仓库里**没有** `sdf_demo` 的 CMake 目标（上面那行就是它唯一的编译方式）。
本节以前写的 `./service/sdf_demo` 是手工编出来的产物，容易让人以为 `cmake --build`
会生成它。

```bash
# 凭据目录里要有 hsm_ca.crt / client.crt / client.key
./sdf_demo 192.168.50.175 ~/.config/pqchsm/pki 9797 axu3egb-hsm-01
#          └ 板子 IP      └ 凭据目录            └ 口  └ 期望的设备证书 CN（可省）
```

⚠️ **现代 OpenSSH 连这块板要两个 `-o`**（只跟 `--provision` 那一步有关）：
`HostKeyAlgorithms=+ssh-rsa` 让它接受板子的 RSA 主机密钥，
`PubkeyAcceptedAlgorithms=+ssh-rsa` 让它愿意用 RSA(SHA-1) 签名认证。
少了后者会得到 `Permission denied (publickey,password)` —— **不是公钥没装**，
而是本机 OpenSSH 默认不再发这种签名。

⚠️ **板上 `pki/` 里三样（`hsm_ca.crt` / `hsm_device.crt` / `hsm_device.key`）
缺一，daemon 就不监听 TCP**（fail-closed）。远程口是 TCP **9797**。

`sdf_demo` **不链接任何密码算法库**——它链的 OpenSSL 只做 TLS 传输——所以它自己
算不出任何东西。它打印出的每一个正确数值都出自 FPGA。

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
