# LAN1 EQM 队列诊断

诊断只在 --eqm-counters 与 --cpu-diagnostic 显式组合下启用，对应 DP 属性 hsan,experimental-eqm-queue-counters，默认行为不增加任何 EQM 事务

ethtool 统计追加 240 项，分别覆盖 LAN1 qid81..88 和 CPU qid129..160，每个队列为 table81 的 word0..3、table80 的 word2 原始值和 valid，共计894项

只在 PIE active 且无 datapath fault 时请求采样，DP 检查所有者、时钟、隔离和队列映射，CSR 保持独立 MMIO 写门控、CPU STOP fence、EQM 互斥和错误隔离，读取表也必须提交命令寄存器，不属于纯寄存器只读访问

任一命令失败立即停止，部分完成的队列只按 valid 标记使用，不重复发命令，全部 DMA 仍由原有生命周期约束保留，不据计数释放或复用

表81与表80分开读取，40个队列也不是同一时刻的快照，原厂调用将 table81 前三词求和用于打印当前队列状态，不能默认这些是单调累积报文计数，零快照不能证明报文从未通过

读取是否清零尚未资格验证，诊断不构成转发、GRO 或 STOP/owner 归还验收

原厂 hi_eqm_read_tab 的30组离线执行证明已确认五词上限，无需增加 CSR API 长度

当前修改的8模块已由 LLVM22.1.8、Linux7.3.5、ARCH=arm64、KCFLAGS=-Werror 编译通过，hsan-net-v300.ko 为120744字节，低于131072字节加载上限，真实构建日志位于 drivers/lan1-eqm-counters/BUILD_EVIDENCE.txt
