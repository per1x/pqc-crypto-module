[English](README.md) · **中文**

# PYNQ 路径

在 PYNQ 镜像上用 Python 驱动加速器。只有真板子才能回答的三个问题 —— 地址对不对、
PL 时钟通不通、结果对不对 —— 走这条路径最快得到答案：改任何东西都只是编辑一个
Python 文件，不需要交叉编译一整套固件。

[`../petalinux/`](../petalinux/README.zh-CN.md) 那条路径才是产品形态。两条路径
遵守同一份寄存器契约（[`docs/register-map.zh-CN.md`](../../../docs/register-map.zh-CN.md)），
因此在这里验证过的时序在 C 侧同样成立。

> 下文凡标注"板上"的内容**都必须在板子上执行**，且一次都没有执行过：
> 本仓库的开发机上没有 Zynq 硬件，也没有装 PYNQ。

## 文件

| 文件 | 作用 |
|---|---|
| `pqc_accel.py` | `PqcAccel` 类：加载 overlay，驱动 AXI4-Lite 寄存器与 AXI-DMA 两个通道 |
| `selftest.py` | 四项上板检查，外加一组不依赖硬件的参考值自检 |

## 比特流怎么放

`build_bitstream.tcl` 在 Vivado 工程的 `outputs/` 下留三个产物：

| 产物 | 谁要用 |
|---|---|
| `pqc_accel_bd_wrapper.bit` | PYNQ 的 `Overlay` |
| `pqc_accel_bd.hwh` | PYNQ 的 `Overlay` |
| `pqc_accel_bd_wrapper.xsa` | PetaLinux |

**PYNQ 要求 `.hwh` 与 `.bit` 同目录、同主名。** Vivado 给交接文件取的是 block
design 的名字（`pqc_accel_bd.hwh`），给比特流取的是顶层封装的名字
（`pqc_accel_bd_wrapper.bit`），所以必须改名对齐：

```sh
# 板上，放 overlay 的那个目录
cp pqc_accel_bd_wrapper.bit  .
cp pqc_accel_bd.hwh          pqc_accel_bd_wrapper.hwh
```

`.hwh` 缺失时 `Overlay()` 直接报错；`.hwh` 过期时 —— 也就是它与 `.bit` 来自不同
的两次构建 —— `Overlay()` 会成功，而地址映射悄悄是错的。`pqc_accel.py` 对第二种
情况留了一道防线：它把从 `.hwh` 拿到的基址与
[`../include/pqc_accel_zynq.h`](../include/pqc_accel_zynq.h) 里的常量比对，不一致
就打警告。不过把这两个文件当成一对、永远一起拷，比事后看这条警告便宜得多。

## 怎么加载 overlay

```python
from pqc_accel import PqcAccel

with PqcAccel("pqc_accel_bd_wrapper.bit") as accel:
    print(f"VERSION = 0x{accel.version:08X}")          # 期望 0x00010000

    out = accel.keccak_f1600(bytes(200))               # 一次置换
    print(out[:8].hex())                               # e7dde140798f25f1，即首个 lane 的小端字节

    coeffs = [0] * 256
    coeffs[0] = 1
    print(accel.ntt(coeffs)[:4])                       # 正变换
    print(accel.ntt(coeffs, inverse=True)[:4])         # 逆变换
```

这个类严格照 `docs/register-map.md` 的命令时序走：武装接收通道 → 送入输入包 →
写 `MODE` → 写 `IN_LEN` → 写 `CTRL = START` → 轮询 `STATUS` 到 `DONE` → 查 `ERR`
→ 读 `OUT_LEN` → 取回结果。改这个顺序之前要知道两件事：

* **接收通道在 `START` 之前武装。** 加速器一算完就把结果往外推，S2MM 还没跑起来
  的话那几拍会因为 `TREADY` 为低而堵在 PL 里。`accel_zynq.c` 里"先武装 S2MM，
  再启动 MM2S"是同一个理由。
* **输入包整包送达之后才写 `START`。** 加速器的语义是"包收齐、写 `START`、再开算"，
  `START` 与数据竞争的结果是算到上一包的残留。

**Cache 一致性在这条路径上不需要做任何事。** `pynq.allocate` 给的是
dma-coherent 的 CMA 缓冲，CPU 侧的访问不经过会与 DMA 别名的缓存，所以
`pqc_accel.py` 一次都没有调用 `flush()` 或 `invalidate()` —— 在 coherent 缓冲上
这两个方法不做事。这也是两条路径唯一在机制上（而非时序上）不同的地方：C 那条
路径靠 `/dev/mem` + `O_SYNC` 把保留内存映射成非缓存，得到同样的效果。

`pqc_accel.py` 里每一次等待都受构造参数 `timeout`（默认 1 秒）约束。PYNQ 自带的
`DMA.wait()` 会一直转下去；在流握手不通的板子上那就变成一个挂死且不带任何信息的
脚本，对第一次上电来说是最糟的结果。

## 依赖

* 一个 Zynq-7000 板的 PYNQ 镜像。本移植的参考板是 PYNQ-Z2。
* `pynq` 版本要能提供 `pynq.allocate` 与 `pynq.lib.dma`。不要照文档里的版本号
  推断，直接在手上的镜像上核对：

  ```sh
  python3 -c "import pynq; print(pynq.__version__)"
  python3 -c "from pynq import allocate; from pynq.lib.dma import DMA; print('ok')"
  ```

* `numpy`，PYNQ 镜像都自带。
* 仓库里的 `hardware/model/ref_model.py`，`selftest.py` 要用。板上没有完整签出时，
  把这一个文件拷到 `selftest.py` 旁边，或者用 `--ref-model` 指出它的目录。
* root 权限：加载比特流要写只有 root 能写的设备。

## 怎么跑 selftest.py

板上：

```sh
sudo python3 selftest.py --bitstream pqc_accel_bd_wrapper.bit
```

开发机上可以只验参考值，不需要任何硬件 —— 这一部分验的是"检查器本身对不对"，
而不是加速器：

```sh
python3 selftest.py --refs-only
```

```
== 参考值自检（不依赖硬件）==
[ 通过 ] 参考值：全零态置换的首个 lane 等于公开常量 —— 得到 0xF1258F7940E1DDE7，期望 0xF1258F7940E1DDE7
[ 通过 ] 参考值：用同一个置换手工算的 SHAKE128("") 与 hashlib 一致 —— 前 8 字节 7f9c2ba4e88f827d / 7f9c2ba4e88f827d
[ 通过 ] 参考值：系数打包/解包往返恒等 —— 512 字节
[ 通过 ] 参考值：正变换结果在 16 位有符号范围内 —— 极值 -1632 .. 1653
[ 通过 ] 参考值：正逆变换往返满足 invntt(ntt(x)) ≡ x·2^16 (mod q)
```

退出码等于失败项数。

### 四项上板检查

顺序是刻意的：前一项不过，后面各项的诊断信息没有意义，所以 `VERSION` 不过时
`selftest.py` 直接停下。

1. **`VERSION` 读到 `0x00010000`。** 只验控制面。
2. **全零态 Keccak-f[1600]。** 验 200 字节这条数据通路。比对的是全部 200 字节，
   不是只看公开的首个 lane `0xF1258F7940E1DDE7`：只查一个 lane 的话，另外 24 个
   全错也能过。
3. **256 点 NTT 正变换。** 验 512 字节这条数据通路，以及"两个 16 位系数打一个
   32 位字"的字节序。
4. **未实现的操作码返回 `ERRCODE = 3`。** 验失败路径。硬件只实现了 7/8/9，其余
   必须如实报错而不是算出点什么来。这一项过不过，决定的是"一个部分实现的加速器"
   与"一个看起来完整的加速器"之间的区别。

第 2、3 项的参考值来自 `hardware/model/ref_model.py`，且参考值本身先被验过一遍：
Keccak 置换用手工走一遍 FIPS 202 的海绵结构与 `hashlib.shake_128` 交叉验证
（168 字节输出 = 21 个 lane 逐字节比较），NTT 用正逆变换的往返关系
`invntt(ntt(x)) ≡ x·2^16 (mod q)` 验证。

## 故障怎么分

三类失败指向互不相交的原因集合。判断自己落在哪一类，诊断就已经做完了大半。

### `VERSION` 不对（`0x00000000`、`0xFFFFFFFF` 或一堆垃圾）

控制面没有到达加速器的寄存器组。此时关于数据面的任何结论都不成立。

* 比特流实际没加载。`Overlay()` 没抛异常不能作为证据，去看
  `overlay.bitfile_name` 与 PL 的状态。
* `.hwh` 与 `.bit` 来自不同的两次构建，`.hwh` 里的地址指向了别的东西。解析出的
  基址与 `0x43C00000` 不一致时 `pqc_accel.py` 会打警告。
* `create_project.tcl` 里改了地址映射，但没同步改
  `include/pqc_accel_zynq.h` 与 `pqc_accel.py` 里的 `ACCEL_BASE`。三处必须一致。
* `FCLK_CLK0` 没使能，或者 `aresetn` 一直压着：没有时钟，AXI4-Lite 的读握手永远
  完不成，表现为挂死或者读回互联替它凑出来的值。
* 挂死而不是读到错值，说明读操作根本没等到响应 —— 那是时钟/复位的问题，
  不是地址的问题。

### `VERSION` 对，但 `DONE` 一直不置位

寄存器是通的，所以问题在命令路径：要么 `START` 没生效，要么加速器没拿到输入。

* `PqcAccelTimeout` 指向**输入（MM2S）**通道：加速器不收数据。`BUSY` 期间
  `TREADY` 为低，所以先怀疑上一条命令把加速器留在了忙状态 —— 先调一次
  `accel.reset()`。仍然不行就怀疑 `axi_dma_0/M_AXIS_MM2S` 到
  `pqc_accel_0/s_axis` 这条流连接。
* `PqcAccelTimeout` 指向 `STATUS.DONE`：输入到了，数据通路没跑起来。100 MHz 下
  最长的命令约 13 µs，1 秒的时限是四个数量级的余量 —— 这绝不会是"还在算"。
  怀疑加速器的 `aclk`/`aresetn`，或者一组被硬件拒绝的 `MODE`/`IN_LEN`
  （那种情况会置 `ERR`，手工读一下 `accel.status` 与 `accel.errcode`）。
* `PqcAccelTimeout` 指向**输出（S2MM）**通道：加速器算完并置了 `DONE`，但结果没
  回来。怀疑 `pqc_accel_0/m_axis` 到 `axi_dma_0/S_AXIS_S2MM` 这条连接，或者
  DMA 经 HP0 到 DDR 这条路。
* `accel.last_poll_count` 用来把"根本没跑"与"跑得慢"分开：计数很大说明轮询循环
  确实是对着一个活的寄存器在转。

### `VERSION` 对、`DONE` 置了，但结果不对

管路是通的，运算不对。`selftest.py` 会打出第一个差异的偏移或下标，这一条信息就
足以定位到下面哪一种：

* **每个字节都不一样**：字节序或打包方式。NTT 的载荷是 256 个 16 位有符号小端
  系数，两个一个 32 位字，低半字在前。
* **结果是上一条命令的输出**：输入在 `START` 之前没进到 PL，或者输出被取了两次。
  结果只能取一次，再取必须重新发命令。
* **零星几个系数不一样**：RTL 里的算术 bug，或者输入超出了各核所声明的范围。
  拿 `hardware/tb/cocotb/test_axi.py` 复现 —— 它对着同一份参考模型比对，且回路
  里没有板子 —— 然后在那边二分。仿真给的是波形，板子给的只是一个错的数。
* **同样的输入两次跑出不同结果**：不是逻辑 bug。要么是 100 MHz 的时序没收敛
  （看 Vivado 构建产出的 `timing.rpt`），要么 —— 只可能在 C 那条路径上 ——
  是 cache 一致性写错了。PYNQ 这条路径不可能是一致性问题，`allocate` 给的缓冲
  本身就是 coherent 的。
