## 扩展56队列与port33映射实板 20261126T115711233391Z

前一RAM候选lan1-extra-queues已验证1078项统计和56队列有效，新增qid161..176全部零，只有LAN1 q81随TX完成10→16，IPS LAN1 row4 raw0从1→17，RX仍0

本轮lan1-port33-opp通过独立 --opp-port33 提交原厂1024词映射并全部读回通过，60秒观察及流量采样无DMA或EQM隔离，TX完成10→16，IPS LAN1 raw0为1→17，RX及qid129..176仍为零快照，双向ICMP失败

这说明恢复该原厂初始化范围仍不足以打通接收，不能反推入站报文以port33为目标，后续继续核对默认CPU转发目标以及port2等剩余原厂映射，禁止以队列名称替代实际路径证据

统计ARMEL helper上限扩为2048并与静态缓冲一致，网卡模块127392字节低于131072加载限制，DMA STOP/owner归还和GRO仍未验收，最终验收0/4

# port33 OPP 原厂映射候选

原厂 hi_pfe_opp_init 的A32执行trace逐字验证1024词，地址0x06058000 + priority*4 + selector*32，priority0..7、selector0..127，值BIT8 | (169+priority)

新增独立 --opp-port33 选项及 hsan,experimental-program-port33-opp 属性，默认关闭，仅与CPU诊断组合，原有flags及默认映射不变

PFE提交前要求CPU OPP选项、DP port33的qid169/count8以及FAM地址、属性、初始化ACK、EQM和PIE映射满足现有交接条件，预检完整4096字节窗口，提交时逐词写入并读回，失败不重试或回滚，沿用现有PFE隔离和硬件生命周期保留

补齐该原厂初始化范围不等于证明port33就是CPU出口，不修改TFRT动态表、不猜CPU转发action、不设置完整PFE forwarding ready，也不以读回替代RX、GRO或STOP/owner归还验收

前一候选已验证1078项统计及56个队列有效，只有LAN1 q81 table81 word3随TX从10增至16，qid129..176全部零快照，IPS LAN1 row4 raw0从1增至17，RX仍0

本候选的实板启动日志已观察到1024词读回通过，后续网络验收单独记录
