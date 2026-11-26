## 调用者溯源修正

原厂 selector7 的已证实调用者是 hi_kcfe_lrn_l2.ko 的 hi_cfe_lrn_l2_add，在0x290设置7并于0x2b0写入flow+0x55，0x3e8调用hi_adp_flow_add

hi_kcfe_pub.ko 的 hi_adp_flow_add @0x19cc读取注册ops+0x38，hi_kcapa.ko的.rodata+0x38经ELF重定位解析为hi_capa_flow_add @0xe6c，其初始化将此ops交给hi_cfe_adp_lsw_register

因此现有证据证明L2学习加速使用该候选，不能把selector7直接称为CPU接收规则，也不能据此断言移植它会修复首包RX

真实action来自学习记录的端口六字节及MAC、direction、QoS、tag、stat助手，并非前一轮离线验证所用的任意测试action，实际CPU收包语义仍需独立证明

已解析原厂九个PUB配置记录的函数重定位，0x101回调nhp_write @0x3ca0、0x102回调l3intf_write @0x3b38、0x103回调tag_write @0x3e1c，当前软件shadow不等于这些回调已执行

0x100由DP内部初始化创建，是24字节计数资源，回调hi_tab_write_cnt/hi_tab_read_cnt，不能把其分配ID当作CPU转发目标

新增inspect_oem_capa_dependencies.py直接校验原厂ELF重定位及关键A32指令，输出oem-capa-callback-provenance.json可独立复核，本轮未修改硬件配置，未声称网络恢复

后续优先追踪首次入站的CPU trap或默认转发入口及PFE各级活动证据，L2学习路径保留用于后续完整加速移植，避免用学习加速替代基础CPU收包

# CAPA selector7 完整候选及前置依赖

执行原厂 hi_kcapa.ko 的 hi_capa_flow_add @0xe6c，selector7 分支 @0x1114，已核对完整32字节 table0x11b 候选，不再仅有16字节前缀

36组候选构造覆盖三组准备元数据、untagged 0x1000和VID37、入口选择0/10/15、lookup命中与未命中后分配，真实A32执行结果与逐字节期望全部一致

另有4组执行真实内部准备函数0x4dc及其0x208/0xf0子函数，仅替换外部table manager为显式模型，核对以下调用序列及分配数据

1. table0x100 alloc_direct，24字节，返回值写入中间状态的单字节字段
2. table0x102 alloc_hash，20字节，返回ID低6位进入中间状态
3. table0x101 alloc_hash，12字节，返回ID低10位进入中间状态
4. table0x103 alloc_hash，12字节，准备路径数据为000000000000000008000000
5. table0x11b get_id，32字节，未命中时alloc_hash相同候选，输出handle加0x40000000标志

准备函数输出四词的来源为 table0x101 ID、table0x102 ID、table0x100 ID、table0x103 ID，最终候选仅使用其中各自低9、低5、低9、低10位，不能把任意占位ID当成真实硬件资源

候选offset26半字 = (metadata1 & 31) | ((metadata0 & 511) << 5)，offset28词 = (metadata2 & 511) | ((metadata3 << 9) & 0x7fe00)，offset20词还依赖action字节8低2位与字节9低3位

本次证明使用显式action测试数据，未证明该输入就是LAN1所需CPU转发动作，selector7名称本身不能当作完整CPU路径语义，尚需追溯调用者实际传入的action

当前移植仅有部分PUB表shadow、一个默认tag和IFC初始化，不能由它们推断这些动态资源已存在，缺失的前置分配和实际硬件回调需要完整移植后才能形成有效规则

离线模型不会写CSR、不在板端安装规则，不构成RX、GRO或STOP/owner归还验收，当前网络仍未上线

复现入口 trace_oem_capa_selector7.py --module <hi_kcapa.ko> --output <JSON>，输出中的 models、unresolved 和 preparation_executed_cases 区分被执行原函数与被替换外部依赖
