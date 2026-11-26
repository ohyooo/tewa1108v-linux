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

驱动顺序、模块参数和板端诊断由 `tools/ram-boot/driver-test.c`、`module-pins.inc` 和 `diagnose.sh` 记录，新编译后需按实际 KO 字节数更新模块大小，仓库内现有pins只对应已记录的候选产物

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

原厂KO需由设备备份中自行取得，原厂二进制未放入本仓库

离线工具显式建模 hash索引分配、初始寄存器值和IFC busy/error状态，同时实际执行原厂A32的IFC初始化和tag_write回调，输出不构成实板硬件ACK或网络验收

## RAM启动材料

`tools/ram-boot/build_lsw_diagnostic_pair.py` 需要配套的基线FIP/initrd、实际Image、八模块和ARMEL helper，原厂BL31、引导stub与overlay的保留边界由脚本检查

该仓库提供重建源码和说明，不包含完整基线固件或Debian根文件系统，不能只编译KO后对任意OEM内核强制加载

驱动使用现有内核输出目录增量构建，当前进度见 [roadmap](docs/lan1-progress.md)

## 只读实板计数器采样

`tools/ram-boot/lan1-ethtool-read.c` 为 freestanding ARMEL helper，仅使用标准 SIOCETHTOOL 查询驱动计数器，不读写用户态MMIO，不up/down或重新加载模块

```sh
clang --target=arm-linux-gnueabi -march=armv5te -marm -O2 \
  -fno-builtin -fno-stack-protector -fuse-ld=lld -static -nostdlib \
  -Wl,-e,_start -Wall -Wextra -Werror \
  tools/ram-boot/lan1-ethtool-read.c -o out/lan1-ethtool-read.elf
```

## 通信验证工具

设备需运行配套驱动并配置测试地址，`lan1-network-test.c` 可设置测试地址并执行 ICMP 验证，TCP、UDP 工具用于直连测试网口

```sh
mkdir -p work
clang --target=arm-linux-gnueabi -march=armv5te -marm -O2 \
  -fno-builtin -fno-stack-protector -fuse-ld=lld -static -nostdlib \
  -Wl,-e,_start -Wall -Wextra -Werror \
  tools/ram-boot/lan1-udp-test.c -o work/lan1-udp-test.elf
```

Windows 主机使用已有 pyserial 环境，在管理员 PowerShell 中运行 `test-lan-tcp.ps1` 或 `test-lan-udp.ps1`，参数 `-PythonPath` 指定解释器，串口和测试网段见脚本配置

UDP 工具包含分片报文验证，TCP 工具使用逐字节回传比较，运行日志仅写入本地 `work/`

UDP 验证可用 `-Rounds` 指定回传次数，范围 1..20000，默认 1000

`--retain-dma-on-down` 启用保留映射的软暂停恢复，编译并上传 `lan1-interface-test.c` 对应程序至设备 `/tmp/lan1-interface-test` 后，可用 TCP 工具的 `-InterfaceCycles` 验证传输期间 down/up，仍不允许卸载模块或释放 DMA 映射

`--napt-partition` 在完整 CPU 诊断配置上启用原厂 NAPT 分区初始化，要求匹配的独立保留内存和 MCC 寄存器读回，仅完成初始化命令检查，尚未安装硬件流规则

`--gro-resources` 配合 `--napt-partition` 初始化 GRO 所需的 L3/NHP 资源，比较既有表项后按原厂顺序写入并读回，尚不安装流规则
