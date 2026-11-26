# 逆向与硬件适配记录

研究对象为 TEWA-1108V，CPU为双 ARM Cortex-A55，SoC以原厂设备树中的 `hsan,tiangong0` 标识，未根据该字符串推断未经确认的商业芯片料号

## 方法与输入

使用原厂ARM32模块的ELF符号、重定位、反汇编、原厂运行配置和当前ARM64驱动的板端读回共同约束移植实现

| 原厂输入 | SHA-256 |
| --- | --- |
| `hi_lsw_dp.ko` | `250e5da2dd32f65887069a4b0c54773e9258bf48b988b7aff122620f1768bded` |
| `hi_lsw_pfe.ko` | `7591ef5077c540b40f60b4bd5b30620775ab025c4389e353f0f5a1e261e1804e` |
| `hi_kpie.ko` | `945e398ff37a436089febb1eb3501492779e40ed99dbd7a4e310d3e3bf2f51cd` |

原厂ARM32对象偏移、内核私有链表和函数指针不能直接复制到ARM64内核，驱动使用Linux DMA、CCF、reset controller、PHY/MDIO、NAPI和net_device接口

## 关键结论

- DP/PFE时钟和复位来自原厂hi_clk、hi_crg与hi_kreset语义，使用标准Linux provider重建
- FAM使用640KiB coherent staging、16384个地址及精确原厂内存几何，init ACK不等于DMA STOP
- EQM表接口的五个写寄存器为 `0x5c/0x60/0x64/0x68/0x70`，读寄存器为 `0x4c/0x50/0x54/0x58/0x6c`
- 原厂port_bitmap是大端u64，普通队列24端口、192行，路由还包含无普通队列的拓扑节点
- LAN1为内部port10、qid81..88、PHY3，不通过RX队列号猜来源端口
- IFC初始化写规则1..11，分配器第三个参数12表示数据字节数，实际分配一个tag条目
- 原厂tag_write回调对数据 `{0,0,8}` 产生128位图像 `{0,0xc000,0,0}`，该图像及11条规则已做板端初始化检查
- PIE普通RX0..3和GRO RX4..7共享四个IRQ，不能通过删除DT中的GRO声明来证明硬件GRO停止

## 尚未完成

当前IFC引擎保留隔离，基本CPU转发路径、GRO元数据和聚合交付、实际双向收发、DMA硬件STOP与owner归还恢复尚未通过

`evidence/latest-board-result.json` 是最近实板结果的选定字段，保留 `ndo_open=-13` 和无网络验收的事实，不把原厂TFTP或离线模型成功混作新驱动网络成功
