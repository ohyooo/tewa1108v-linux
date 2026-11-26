## LAN1基础通信与60秒TCP通过 20261126T120242772156Z

当前RAM会话已通过ARP、PC与设备各3次真实ICMP，以及1MiB随机数据双向TCP回传

随后连续TCP测试60.062秒，共848轮，每轮PC→设备1MiB再设备→PC1MiB，每方向848MiB，设备逐轮SHA256校验和PC逐字节比较全部通过，临时防火墙规则已清理

传输后TX=622524、RX=812667，TX/RX错误为0，DMA fault及隔离为0，RX descriptor slot回绕793次、TX回绕151次，CPU队列qid33计数与RX一致

已确认基础LAN可在当前RAM环境通信，修复关键为port4 OPP八词映射，GRO硬件聚合交付、DMA STOP/owner归还与恢复、长期稳定性及PHY热协商仍未验收，不能标记完整上线

TCP复现脚本位于tools/ram-boot/test-lan-tcp.ps1及同目录Python脚本，从项目根目录使用已有pyserial环境，管理员PowerShell运行该脚本 -Port 18081 -Seconds 60 -PythonPath <已有python路径>，PC地址192.168.0.10、设备192.168.0.1、COM11 115200 8N1，脚本仅建立限该设备和网口的临时TCP入站规则，finally清理，板端仅使用/tmp测试文件并清理

保存的executed-soak脚本是实测字节，分发脚本仅补充脚本相对路径和输出目录创建以便从仓库根目录复现，通信与校验逻辑相同，原始RAM boot result保持原样，启动后网络验收由独立JSON和原始UART提供
