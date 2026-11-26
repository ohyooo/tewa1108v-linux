# TEWA-1108V Linux

TEWA-1108V 的 Linux 7.3.5 ARM64 适配、LAN1 驱动和原厂模块逆向研究

当前支持 RAM 启动与八模块加载，LAN1 基础通信已通过，完整上线仍在推进，见 [roadmap](docs/lan1-progress.md)

## 硬件范围

| 项目 | 当前确认值 |
| --- | --- |
| 设备 | TEWA-1108V ONU/ONT |
| SoC | 设备树兼容标识 `hsan,tiangong0`，商业芯片料号尚未确认 |
| CPU | 双核 ARM Cortex-A55，AArch64；当前启动实验使用 `maxcpus=1` |
| RAM | 512 MiB，物理范围 `0x80000000..0x9fffffff` |
| 原厂内核 | Linux 5.10，ARM32 模块 ABI |
| 适配内核 | Linux 7.3.5，ARM64，保留 ARM32 compat 支持 |
| 用户态 | Debian 13 ARMEL，EABI soft-float |
| LAN1 | GEMAC `0x14400000`，内部端口10，GMII |
| MDIO / PHY | MDIO `0x14600000`，PHY地址3，ID `0x20669a22` |
| PIE | `0x12080000`，普通RX0..3，GRO RX4..7，四组共享中断 |
| 构建工具链 | LLVM/Clang 22.1.8 |

## 文件

- `kernel/linux-7.3.5/`：完整内核源码，保留原始文件模式和符号链接；与构建机的96,081个唯一文件核对见 `kernel/source-audit.json`
- `kernel/configs/tewa1108v.config`：当前实板内核对应配置，包含 DW WDT 和 reset controller
- `drivers/lan1/`：PIE、GEMAC、MDIO、LSW CSR/DP/PFE/platform 和 CRG 驱动，共八个外置模块
- `reverse/disassembly/`：原厂模块反汇编和符号研究材料
- `reverse/profiles/`：队列几何、IFC编码图像和离线 A32 执行工具
- `reverse/reference/`：原厂系统中采集的队列、内存、PIE和表配置参考
- `tools/ram-boot/`：FIP/DTB/initramfs 处理及 ARMEL 诊断源码
- `evidence/`：编译记录和描述符逆向依据，详细运行记录保留本地

编译步骤见 [BUILD.md](BUILD.md)，逆向结论和证据边界见 [docs/reverse-engineering.md](docs/reverse-engineering.md)

## 开发状态

进度与待办统一记录在 [roadmap](docs/lan1-progress.md)，详细实板日志保留本地

当前仅有 RAM 启动验证，尚不提供正式 NAND 刷写镜像

## 源码来源

内核输入为 `linux-7.3.5.tar.xz`

内核原始版权和许可证保留在内核目录，驱动文件中的 SPDX 标识按文件适用，本仓库不包含整机 NAND 备份、原厂文件系统、登录密码、SSH凭据或LLVM工具链二进制
