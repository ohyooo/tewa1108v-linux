# LAN1 实板进展 2026-11-26

已移植并实板验证普通RX0..3与GRO RX4..7各自的DMA所有权、四组配对IRQ、PIE七个全局字段与24个阈值、原厂IPS八个队列寄存器、IPS最终使能及OFC控制字段

LAN1可UP、ndo_open=0、1000Mbps全双工，但TX低16位completion始终为0，约37秒后code7隔离并保留DMA映射，最终网络验收0/4

原厂hi_pie_tx_get_rptr明确取低16位，高16位跟随发布位置不能当作完成并回收

原厂IPS使能移至FAM初始化之前，板端结果未改变，PIE 14项TX计数和11组IPS计数均为0，首描述符仍硬件持有

新增非一致性DMA诊断候选：构建器--noncoherent-dma删除设备树dma-coherent并要求Linux实际按非一致性DMA处理，ACP九个属性保持原值，实板同样未解决TX停滞，此候选仅诊断用途

GRO聚合报文交付、双向ARP/ICMP/TCP、DMA STOP与owner归还、持久上线均未验收，不刷NAND，不通过强制释放DMA或伪造completion绕过失败

驱动使用现有Linux 7.3.5内核输出目录及LLVM22.1.8完成Kbuild KCFLAGS=-Werror，无新增第三方依赖

原始逆向材料是寄存器布局、队列几何和原厂流程的重要依据，部分参考代码缺少实际硬件初始化和生命周期实现，必须结合原厂A32反汇编与实板反馈补齐
