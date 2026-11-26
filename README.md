## LAN1基础通信与60秒TCP通过 20261126T120242772156Z

当前RAM会话已通过ARP、PC与设备各3次真实ICMP，以及1MiB随机数据双向TCP回传

随后连续TCP测试60.062秒，共848轮，每轮PC→设备1MiB再设备→PC1MiB，每方向848MiB，设备逐轮SHA256校验和PC逐字节比较全部通过，临时防火墙规则已清理

传输后TX=622524、RX=812667，TX/RX错误为0，DMA fault及隔离为0，RX descriptor slot回绕793次、TX回绕151次，CPU队列qid33计数与RX一致

已确认基础LAN可在当前RAM环境通信，修复关键为port4 OPP八词映射，GRO硬件聚合交付、DMA STOP/owner归还与恢复、长期稳定性及PHY热协商仍未验收，不能标记完整上线

TCP复现脚本位于tools/ram-boot/test-lan-tcp.ps1及同目录Python脚本，从项目根目录使用已有pyserial环境，管理员PowerShell运行该脚本 -Port 18081 -Seconds 60 -PythonPath <已有python路径>，PC地址192.168.0.10、设备192.168.0.1、COM11 115200 8N1，脚本仅建立限该设备和网口的临时TCP入站规则，finally清理，板端仅使用/tmp测试文件并清理

保存的executed-soak脚本是实测字节，分发脚本仅补充脚本相对路径和输出目录创建以便从仓库根目录复现，通信与校验逻辑相同，原始RAM boot result保持原样，启动后网络验收由独立JSON和原始UART提供

## LAN1真实双向ICMP首次通过 20261126T120242772156Z

新增port4 OPP八词映射并逐词读回后，PC→设备3次ping全部成功、延迟小于1ms、丢包0%，设备→PC的3次ICMP回复通过源目的地址、ID、序号、payload和checksum校验，板端ARP已解析

实板RX计数1→13，TX完成10→18，qid33 table81 word3与RX同步，64队列统计有效，采样期间DMA未隔离

关键缺项为原厂逻辑port4的0x06042880+priority*4映射到qid33..40，之前仅设置port40..43虚拟映射不足，原厂hi_capa_set_cpu_rate固定选择port4的调用证据与此次实板结果一致

这是RAM诊断模式的基础收发通过，TCP、长期稳定性、GRO聚合交付、DMA STOP/owner归还恢复和正式上线仍未验收，不能标记全部完成

boot工具原始result中的CPU_LAN1_UP_TRAFFIC_NOT_ACCEPTED保留其启动阶段结论，启动后的独立流量验收以port4-acceptance.json及原始网络测试记录为准，不回改历史result

# TEWA-1108V Linux

TEWA-1108V 的 Linux 7.3.5 ARM64 适配、LAN1 驱动和原厂模块逆向研究

当前可以 RAM 启动 Linux 7.3.5、运行 Debian ARMEL 并加载八模块，LAN1已UP、1000Mbps全双工，修正描述符首尾标志后TX completion正常推进且无DMA隔离，RX仍为0，ARP和双向ICMP未通过，最终网络验收为 `0/4`

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
- `evidence/`：编译记录和裁剪后的实板结果，不含设备登录凭据

编译步骤见 [BUILD.md](BUILD.md)，逆向结论和证据边界见 [docs/reverse-engineering.md](docs/reverse-engineering.md)

## 实板状态

最近确认会话为 `20261126T003926780004Z`

- FAM 初始化 ACK、192条普通队列、25个路由节点、8条辅助队列和160条默认整形配置已验证
- PIE冷复位、8槽位映射、ACP门控及9个缓存属性已验证
- LAN1 OPP优先级到队列81..88、CPU口0与LAN1口10的PUB/VLAN0配置已读回
- IFC tag条目和规则1..11已完成初始化检查，IFC仍保留隔离状态
- 八模块哈希/Live、PHY3、WDT绑定和60秒无复位观察已通过
- 网络双向通信、GRO交付、DMA STOP及owner归还恢复仍未通过

原厂系统正常联网、U-Boot TFTP成功、寄存器读回通过，均不能替代新驱动的网络验收

当前仅有 RAM 启动验证，不提供已完成验收的 NAND 刷写镜像或通用刷机步骤

## 源码来源

内核输入为 `linux-7.3.5.tar.xz`，SHA-256为 `8f293f83d54f0b7b9be56863ebc5e4ea79d73e530b4ae55c572be97ef39f0990`

内核原始版权和许可证保留在内核目录，驱动文件中的 SPDX 标识按文件适用，逆向材料记录其原厂输入哈希；本仓库不包含整机 NAND 备份、原厂文件系统、登录密码、SSH凭据或LLVM工具链二进制
