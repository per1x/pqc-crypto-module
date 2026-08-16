#!/bin/bash
# 变体 C = 变体 A（puf4kmode + shutter，不加 RSA）+ **修正后的辅助数据**。
#
# 修正依据（Xilinx 自己的源码，不是猜）：
#   xilskey_v6_9/src/xilskey_eps_zynqmp_puf.c 注册函数结尾：
#       InstancePtr->Aux                    = (PufStatus & AUX_MASK) >> 4;
#       InstancePtr->SyndromeData[386-1]    = (PufStatus & AUX_MASK) << 4;
#   即：结构体里的 Aux 字段与辅助数据最后一字**差 8 位移位**。
#   上一轮把 Aux 字段值（0x00864FE2）直接写进了最后一字，正确值是 0x864FE200。
#   （CHASH 在 word[383] 与 word[384] 各出现一次，是库本身的行为，不是错误：
#     4K 模式循环停在 Index=383，最后读到的那一字就是 CHASH，随后又被复制到 [384]。）
set -e
cd /home/build/pufboot
source /tools/Xilinx/Vitis/2020.1/settings64.sh >/dev/null 2>&1

cat > p_C.bif <<'BIF'
the_ROM_image:
{
	[fsbl_config] pufhd_bh, puf4kmode, shutter=0x0100005E
	[puf_file] /home/build/pufboot/puf_hd_fixed.txt
	[keysrc_encryption] bh_blk_key
	[bh_keyfile] /home/build/pufboot/black_key.txt
	[bh_key_iv] /home/build/pufboot/bh_key_iv.txt
	[bootloader, destination_cpu=a53-0, encryption=aes, aeskeyfile=/home/build/pufboot/gen.nky] /home/build/petalinux/images/linux/zynqmp_fsbl.elf
	[pmufw_image] /home/build/petalinux/images/linux/pmufw.elf
	[destination_device=pl, encryption=aes] /home/build/wdt_patch/images/pl_fanquiet.bit
	[destination_cpu=a53-0, exception_level=el-3, trustzone, encryption=aes] /home/build/wdt_patch/atf_secmmio/zynqmp/debug/bl31/bl31.elf
	[destination_cpu=a53-0, exception_level=el-1, trustzone, encryption=aes] /home/build/wdt_patch/images/tee_load_quiet.elf
	[destination_cpu=a53-0, load=0x00100000] /home/build/petalinux/images/linux/system.dtb
	[destination_cpu=a53-0, exception_level=el-2, encryption=aes] /home/build/petalinux/images/linux/u-boot.elf
}
BIF

rm -f BOOT_PUF_C.BIN
bootgen -arch zynqmp -image p_C.bif -o BOOT_PUF_C.BIN -w on

python3 - BOOT_PUF_C.BIN <<'PY'
import struct,sys
d=open(sys.argv[1],'rb').read()
w=lambda o: struct.unpack_from('<I',d,o)[0]
be=lambda o: struct.unpack_from('>I',d,o)[0]
ok=True
def chk(n,g,e):
    global ok; good=(g==e); ok&=good
    print(f"  {'OK ' if good else 'BAD'} {n}: {g:#010x} (期望 {e:#010x})")
print("启动头自检:")
chk("keysrc(0x28) bh_blk_key", w(0x28), 0xA35C7C53)
chk("shutter(0x6c)",           w(0x6c), 0x0100005E)
a=w(0x44)
print(f"  ..  attrs(0x44)={a:#07x}  puf_hd[7:6]={(a>>6)&3} bh_rsa[15:14]={(a>>14)&3} puf_mode[17:16]={(a>>16)&3}")
for n,v,e in [("PUF_HD=BH",(a>>6)&3,3),("PUF_MODE=4K",(a>>16)&3,3)]:
    good=(v==e); ok&=good; print(f"  {'OK ' if good else 'BAD'} {n}: {v} (期望 {e})")
bk=d[0x4c:0x6c].hex(); exp=open('black_key.txt').read().strip().lower()
print(f"  {'OK ' if bk==exp else 'BAD'} 黑钥(0x4c)"); ok&=bk==exp
iv=d[0xac:0xb8].hex(); eiv=open('bh_key_iv.txt').read().strip().lower()
print(f"  {'OK ' if iv==eiv else 'BAD'} 黑钥IV(0xac)"); ok&=iv==eiv
red=bytes.fromhex("96387156F04934BC42269938226DD91F144F9BEA92153B018795274CD7D7CA4E")
print(f"  {'OK ' if red not in d else 'BAD'} 镜像中不含红钥明文"); ok&=red not in d
# 辅助数据整块逐字对照
hd=[int(l,16) for l in open('puf_hd_fixed.txt') if l.strip()]
base=0x8B8
mism=[i for i,v in enumerate(hd) if be(base+4*i)!=v]
print(f"  {'OK ' if not mism else 'BAD'} 辅助数据 386 字与 puf_hd_fixed.txt 逐字一致"
      + (f"（不符 {len(mism)} 处，首个 idx={mism[0]}）" if mism else ""))
ok &= not mism
print(f"  ..  word[383]={be(base+4*383):#010x} word[384]=CHASH {be(base+4*384):#010x} word[385]=AUX<<4 {be(base+4*385):#010x}")
print("自检结果:", "全部通过" if ok else "有不符项")
sys.exit(0 if ok else 1)
PY
ls -l BOOT_PUF_C.BIN; md5sum BOOT_PUF_C.BIN
