# 编译

## 环境

使用原生 Linux x86_64 构建主机和 LLVM/Clang 22.1.8，已执行过的构建主机为 Ubuntu 26.04，内核目标为 ARM64，诊断程序目标为 ARMEL

需要 `make`、Clang/LLD/LLVM工具、`flex`、`bison`、`bc`、`pkg-config`、OpenSSL和ELF开发库，以及 Python3、CPIO、gzip和xz工具

将 LLVM 的 `bin` 加入当前会话 PATH，下面命令从仓库根目录执行

## 内核

```sh
mkdir -p out/kernel
cp kernel/configs/tewa1108v.config out/kernel/.config
make -C kernel/linux-7.3.5 O="$PWD/out/kernel" ARCH=arm64 LLVM=1 LLVM_IAS=1 olddefconfig
make -C kernel/linux-7.3.5 O="$PWD/out/kernel" ARCH=arm64 LLVM=1 LLVM_IAS=1 -j"$(nproc)" Image vmlinux modules
```

主要产物为 `out/kernel/arch/arm64/boot/Image`、`out/kernel/vmlinux` 和 `out/kernel/Module.symvers`

完整保留配置中的 ARM32 compat、watchdog和reset controller选项，ARMEL用户态依赖 compat 支持

位于Git仓库中构建时需核对 `make kernelrelease` 的实际输出与模块 vermagic，勿混用不同release或不同配置产生的Image和KO

## LAN1八模块

```sh
make -C kernel/linux-7.3.5 O="$PWD/out/kernel" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
  M="$PWD/drivers/lan1" KCFLAGS=-Werror -j"$(nproc)" modules
```

产物包括 `hsan-net-v300.ko`、`hsan-mdio.ko`、`hsan-lsw-csr.ko`、`hsan-lsw-platform.ko`、`hsan-lsw-crg.ko`、`hsan-lsw-dp.ko`、`hsan-lsw-pfe.ko` 和 `hsan-gemac-lan1.ko`

驱动顺序、模块参数和板端诊断由 `tools/ram-boot/driver-test.c`、`module-pins.inc` 和 `diagnose.sh` 记录，新编译后必须重新生成模块大小和SHA-256 pin，仓库内现有pins只对应已记录的候选产物

## ARMEL诊断程序

```sh
clang --target=arm-linux-gnueabi -march=armv5te -marm -O2 \
  -fno-builtin -fno-stack-protector -fuse-ld=lld -static -nostdlib \
  -Wl,-e,_start -Wall -Wextra -Werror \
  tools/ram-boot/driver-test.c -o out/driver-test.elf
```

PID1、reboot和gate helper源码位于相同目录，`driver-test.c` 使用 `GATE_ONLY` 宏可编译仅执行一次网卡UP ioctl的辅助程序

## 离线逆向工具

```sh
python3 -m venv .venv
.venv/bin/python -m pip install unicorn==2.1.4 pyelftools==0.33
.venv/bin/python reverse/profiles/trace_oem_ifc.py \
  --module /path/to/original/hi_lsw_pfe.ko --output out/oem-ifc-trace.json
```

原厂KO需由设备备份中自行取得并核对原始哈希，原厂二进制未放入本仓库

离线工具显式建模 hash索引分配、初始寄存器值和IFC busy/error状态，同时实际执行原厂A32的IFC初始化和tag_write回调，输出不构成实板硬件ACK或网络验收

## RAM启动材料

`tools/ram-boot/build_lsw_diagnostic_pair.py` 需要与源码中固定哈希匹配的基线FIP/initrd、实际Image、八模块和ARMEL helper，原厂BL31、引导stub与overlay的保留边界由脚本检查

该仓库提供重建源码和说明，不包含完整基线固件或Debian根文件系统，不能只编译KO后对任意OEM内核强制加载

已有构建证据见 `evidence/driver-build.log`，本次仓库整理没有重新执行完整内核编译或新的实板网络验收

## 只读实板计数器采样

`tools/ram-boot/lan1-ethtool-read.c` 为 freestanding ARMEL helper，仅使用标准 SIOCETHTOOL 查询654项驱动计数器，不读写用户态MMIO，不up/down或重新加载模块

```sh
clang --target=arm-linux-gnueabi -march=armv5te -marm -O2 \
  -fno-builtin -fno-stack-protector -fuse-ld=lld -static -nostdlib \
  -Wl,-e,_start -Wall -Wextra -Werror \
  tools/ram-boot/lan1-ethtool-read.c -o out/lan1-ethtool-read.elf
```

最新PA CPU诊断候选在IFC tag/11规则窗口读回、LAN1 OPP/PUB和FAM CPU fence检查通过后提交原厂CSR0x07000704=1，仍保留IFC不确定状态，不宣称完整硬件转发或STOP验收，当前实板验证进行中

原厂136项DROP原因计数和8个RX环完整游标已加入只读统计，标准SIOCETHTOOL helper最多容纳1024项

`--lan1-mirror-cpu` 是独立隔离实验，执行原厂hi_port_set_mirror(10,32,16)对应六次寄存器写，目标16由队列几何推导，未证明为可用接收通道，默认不启用，不作为正常LAN或生产配置，原厂A32执行轨迹见reverse/profiles/oem-mirror-candidate-trace.json
