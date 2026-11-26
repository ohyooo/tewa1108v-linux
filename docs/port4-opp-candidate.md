## LAN1真实双向ICMP首次通过 20261126T120242772156Z

新增port4 OPP八词映射并逐词读回后，PC→设备3次ping全部成功、延迟小于1ms、丢包0%，设备→PC的3次ICMP回复通过源目的地址、ID、序号、payload和checksum校验，板端ARP已解析

实板RX计数1→13，TX完成10→18，qid33 table81 word3与RX同步，64队列统计有效，采样期间DMA未隔离

关键缺项为原厂逻辑port4的0x06042880+priority*4映射到qid33..40，之前仅设置port40..43虚拟映射不足，原厂hi_capa_set_cpu_rate固定选择port4的调用证据与此次实板结果一致

这是RAM诊断模式的基础收发通过，TCP、长期稳定性、GRO聚合交付、DMA STOP/owner归还恢复和正式上线仍未验收，不能标记全部完成

boot工具原始result中的CPU_LAN1_UP_TRAFFIC_NOT_ACCEPTED保留其启动阶段结论，启动后的独立流量验收以port4-acceptance.json及原始网络测试记录为准，不回改历史result

# port4 OPP 与原厂CPU整形接口

原厂hi_capa_set_cpu_rate @0x5814把输入参数原样转发给hi_queue_set_egr_shaping，首参数固定为逻辑port4，五组A32离线执行已验证该选择，输入参数在模型中只作为opaque bits转发，不代表数值速率或有效配置指针

原厂hi_pfe_opp_init对应port4的8词为0x06042880+priority*4，值BIT8 | (33+priority)，已逐字匹配原厂执行trace

新增独立 --opp-port4 与 hsan,experimental-program-port4-opp，默认关闭，需CPU诊断及已有CPU OPP条件，要求DP实际qid33/count8及完整FAM/EQM/PIE前置条件，再预检32字节窗口，逐词写入并读回

诊断新增qid33..40，覆盖64个队列，ethtool总计1126项，ARMEL工具上限2048，网卡模块128928字节，低于131072加载限制

本候选不改变CPU出口整形参数，不提交任何猜测的速率或动态action，不据接口命名认定完整接收路径，不放宽GRO、DMA STOP/owner归还与隔离限制
