## IPS入口实板 20261126T114727353855Z

新RAM候选lan1-ips-counters已通过编译、八模块加载及60秒观察，982项统计读取成功，10行IPS完整采样有效，row8按原厂selector64限制保持无效，采样期间DMA和EQM未隔离

同一真实双向ICMP测试窗口，TX完成10→16，IPS row0 raw0同步10→16，LAN1 port10对应row4 raw0为1→17，静置复采分别仍为16和17，六种IPS丢弃计数全0，RX仍0，双向ICMP失败

IFC、L2、PA、OPP和OFC原始活动计数同窗口11→33，增量22，与IPS两行增量6+16一致，说明排查范围应继续收敛到入口活动之后的处理或CPU出口，不能将这些计数本身当作完整转发验收或穷尽全部硬件丢弃原因

CPU qid129..160依然零快照，LAN1 qid81同步推进，下一步扩大观察到原厂有效的port2/qid161..168及port33/qid169..176并核对对应OPP映射，这些端口在原厂队列profile中存在，但尚未证明其在本机收包路径的实际职责，禁止直接猜测或写入转发目标

读取后静置值保持提供了本窗口未立即清零的证据，不构成所有计数器clear-on-read语义的全面资格验证，最终验收仍0/4

# IPS 入口与丢弃计数诊断

来源为原厂 hi_lsw_dp.ko ips_diag_get_cnt @0x63b0，真实A32离线执行确认72次读取，模型只替换hi_lsw_read返回值和打印，不写硬件

寄存器布局为0x4400101c + row*28 + field*4，11行原厂selector依次为4、3、8、9、10、11、12、0、64、13、20，LAN1 port10对应row4

field0保留raw0名称，不在缺乏实板资格验证时假定其为单调累计入站报文数，field1..6从原厂打印字符串对应sop_drop、eop_drop、exlong_drop、exshort_drop、mac_err_drop、burr_drop

row8的selector64不能通过原厂u32掩码选中六个错误字段，驱动完全跳过该行并保留valid=0，不把未采样的零值当作无丢包

ethtool追加11组8项，共88项，合计982项，低于ARMEL读取工具1024项上限，只在CPU诊断显式开启且PIE active、无datapath fault、CPU fence未关闭时采样

每行七次读取全部成功才置valid=1，任意读取失败即停止，不重试，不发诊断命令，不改变DMA ownership，不代表寄存器是同时刻锁存快照，clear-on-read行为仍未资格验证

候选继承已验证TX首尾标志、队列计数、GEMAC silent步骤及RX0大缓冲实验，不新增转发配置，目标是缩小RX为0的断点
