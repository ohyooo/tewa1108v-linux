## GEMAC silent 原厂流程补齐与实板 20261126T114227535798Z

原厂hi_mac_link_up @0x44ec..0x44f4通过ops+0x44调用hi_mac_set_silent(0)，对应hi_kmac.ko .data+0x44的ELF重定位指向.text+0x54，底层hi_reg_hw_write @0x399c向GEMAC+0x48完整写入32位0

真实执行原厂回调和底层写函数的五组输入验证通过，已补齐该步骤，并把该字段加入初始化读回及运行期间漂移检查，仍保留一次性编程、失败隔离和DMA映射保留限制

新RAM候选编译及60秒观察通过，板端日志显示silent before=0 after=0，因此虽修复流程遗漏，但不能认定其导致了本次RX故障

流量前后TX完成9→16，RX仍0，双向ICMP失败，DMA及EQM采样时未隔离，最终验收仍0/4

下一步采集原厂ips_diag_get_cnt @0x63b0的逐端口入口及六种丢弃计数，寄存器为0x4400101c+row*28+field*4，LAN1 port10对应row4，先用实际增量定位入口，不能把MAC原始计数直接当完整交换转发证明

