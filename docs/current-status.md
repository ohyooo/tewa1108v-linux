## 交接前最新状态与 EQM 读取边界

2026-11-26T11:14:37Z 只读复核：当前会话 20261126T070342079499Z 已在内核运行约 11544 秒后因 PHY3 重新协商超出固定 GEMAC 模式触发 code11，TX submitted 23 / completed 22，RX 0，dma_active=0，dma_quarantined=1，全部 DMA 映射保留

历史 60 秒观察通过仅代表当时未隔离，当前禁止 down/up、rmmod 或释放 DMA，下一轮必须正常软件重启后换新 RAM 候选，隔离后被抑制的 MMIO 零值不能当作 STOP 证明

新增原厂 hi_eqm_read_tab @0x1e1c 的真实 A32 离线执行证明，table80/81、qid81/129/160、请求长度0/1/3/5/8共30组全部通过，长度参数单位为32位词且最多读取5词，请求8不会读出8词，输出尾部保持不变，现有五词 CSR API 无需扩展

hi_queue_port_cnt_get @0x4da8 使用 table81 word0+word1+word2、word3 和 table80 word2低15位，后续可用其定位CPU队列断点，计数器是否清零尚未资格验证，不能把该事务称为无副作用采样

离线证明见 reverse/profiles/oem-eqm-read-proof.json，重放入口 trace_oem_eqm_read.py --module <原厂hi_lsw_dp.ko> --output <输出JSON>，模型为立即完成和可辨识的寄存器数据，不代表实板数据或网络验收

最终验收仍0/4，ARP、双向ICMP、TCP、GRO以及DMA STOP/owner归还恢复均未完成

## 最新实板：TX completion 已修复，RX/ARP仍未通过

20261126T060001777376Z 与 20261126T060812983624Z 均验证描述符首尾标志修复后 LAN1 UP、1000Mbps全双工、60秒无TX stall和DMA隔离

原厂hi_pie_tx_send线性路径传入metadata[7]低两位=3，v300 set_desc映射到descriptor byte3 bits6/5，旧0x14遗漏报文边界，修正为0x74，已实际执行原厂A32函数核对完整32字节编码，提交69ce48c93已推送GitHub

第二次会话网络采样：TX完成10→16，硬件RPTR低16位同步10→16，RX仍0，ARP邻居未完成，双向ICMP无回复，不能将TX DMA完成当成网络可用

同窗口原厂GEMAC四对原始计数增量为12、6、962、384，IFC/L2/PA各采样计数增加18，表明需要继续核对内部入口到CPU队列的转发路径，原始计数本身不构成完整转发验收

已构建只读SIOCETHTOOL ARMEL采样工具，未触碰用户态MMIO，Windows仅测试期间临时增加窄ICMP防火墙规则并清理，抓包已停止，未刷NAND或释放可能仍由硬件拥有的DMA

当前验证原厂PA控制CSR0x07000704=1，在独立CPU诊断条件下依赖FAM/队列/PIE映射、LAN1 OPP/PUB、IFC tag和11条规则窗口读回，保留IFC和全功能转发未验收状态

最终验收仍0/4，后续为ARP/双向ICMP/TCP、GRO交付、DMA STOP与恢复、持久上线

以下为历史阶段记录，其中TX stall、DOWN/-13等描述仅适用于当时会话

## 最新实板结论：LAN1 已 UP，但 TX completion 停滞，网络仍未通过

会话 20261126T021632610848Z 已完成 Linux 7.3.5 八模块加载、1000Mbps 全双工、ndo_open=0、8 个 RX ring 发布，以及原厂 IPS 八寄存器和最终使能读回

TX 软件发布6个描述符，硬件 RPTR=0x00060000，原厂规定的低16位 completion 始终为0，运行约37秒后 code7 隔离并保留全部 DMA 映射，连续60秒未复位，不能解释为网络正常

前序会话 20261126T020859533996Z 已验证 PIE 七个持久字段与24个阈值，0x3600 按原厂 W1C/提交序列处理，不要求写值保持

当前正在验证 OFC 控制 CSR0x04000030 的原厂 masked prelude，未知表容量保持未知，不伪造表所有权，最终网络验收仍0/4，后续依次为 TX/RX 实际推进、双向 ARP/ICMP/TCP、DMA STOP 与恢复、持久上线

以下为历史阶段记录，其中 DOWN/-13 等描述仅适用于当时会话

## 当前最新实板结论：LAN1 OPP 和 PUB 已完成，IFC tag 与11条规则初始化已通过

截至会话 20261126T002926951955Z，FAM init ACK、192/192 普通队列、25 个路由节点、8 条辅助队列、160 条默认关闭整形、PIE 冷复位与 8 槽映射、ACP 门控及 9 个缓存属性均已通过实际板端检查

PFE 已改为等待 DP 冷复位和队列初始化完成再写寄存器，原厂 14＋48 行表与全局写入路径已执行，OPP 三个全局字段、LAN1 八个优先级到 qid81..88 的映射，以及 CPU 口0与 LAN1口10 的端口配置和 VLAN0 成员关系已完成硬件读回

上述各轮均完成八模块哈希/Live、PHY3、Bash nonce 与 60 秒无复位观察，原始证据分别保存于 04-Stage161-UART-Evidence 下对应会话，不能将寄存器初始化通过解释为网络通过

LAN1 仍 DOWN、ndo_open=-13、TX/RX=0，最终网络验收仍 0/4，入口转发/GRO/DMA STOP 与 owner 归还恢复尚未验收，不刷写 NAND，不释放硬件可能拥有的 DMA 地址

原厂 A32 模块离线执行得到 IFC 初始化精确序列：分配一个12字节 tag 数据条目，实际11条规则，索引1..11，89次寄存器写和23次128位表写（含 tag 回调），最后 CSR0x118写0，旧移植代码按0..11写12条规则存在错位，且将分配数据长度12误当成12个条目并遗漏 tag 硬件回调，现已修正并通过 LLVM Kbuild/-Werror

IFC 规则1..11已在会话20261126T003422139737Z完成命令和数据窗口读回，仍保留 IFC 引擎隔离；这轮 WDT 汇总状态未确认，原始结果保持不改，同次后补只读采样确认 dw_wdt 绑定、设备号254:0

后续补齐的 tag_write 硬件图像来自实际原厂A32回调执行，内容为 {0,0xc000,0,0}，冷启动前预留一个 Linux tag 条目，带 tag 回调的候选已在会话20261126T003926780004Z通过实板完整读回，IFC 11条命令完成，八模块哈希/Live、WDT绑定、Bash nonce及60秒无复位通过，host cleanup通过；离线模型不冒充板端采样，DMA 启动门控仍保留

后续顺序：验证 IFC 规则及入口转发 → 解决 GRO 的实际硬件队列/交付 → 双向 ARP/ICMP/TCP 实测 → DMA STOP/owner 归还与恢复 → 完成持久上线方案

以下均为前序阶段记录

## 最新实板进展：EQM、队列路由、默认整形和 PIE 冷启动映射已验证

会话 20261125T215102145847Z 已验证 DP 四线复位、FAM 第 1 次轮询 ACK、EQM table19 完整返回图像、192/192 普通队列读回、25 个路由节点与 8 条辅助队列、160 条默认关闭整形配置字，以及 PIE 冷复位后 RX 禁用和 8 个槽位映射数据读回

八模块文件哈希与 Live、PHY3、WDT 注册绑定、Bash nonce、连续 60 秒无复位和主机清理均通过，完整原始证据在 ../../04-Stage161-UART-Evidence/20261125T215102145847Z/

LAN1 仍为 DOWN，ndo_open=-13，TX/RX=0，最终网络验收仍为 0/4，以上结果不证明 PFE 转发、GRO、DMA STOP、owner 归还或恢复，初始化 ACK 不能用于释放 DMA

当前 PFE 初始化顺序修正已通过 LLVM Kbuild 与 -Werror，PFE 写寄存器前等待 DP 冷复位和映射完成，继续进行 RAM-only PFE 全局初始化实板验证

以下为前序阶段记录

## 最新实板进展：FAM 初始化 ACK 已取得，网络通路仍未完成

会话 20261125T184041610331Z 使用新的 640KiB coherent 地址表、精确原厂几何及正常内核驱动完成启动复位和 FAM 初始化，旧 FAM 状态从 0x1 清为 0x0，提交新 DMA 地址后第 1 次轮询观察到 init ACK，DP owner attempted=1、quarantined=0，内存与时钟保留，未释放给 CPU

同次八个模块哈希、Live、LAN1 owner、WDT 注册和绑定、Bash nonce 与 60 秒无复位均通过；LAN1 ndo_open 仍为 -13、TX/RX=0，PIE 数据队列未启动，FAM init ACK 不等于 DMA STOP ACK，不允许据此 unmap、卸载或 down/up 重用地址

证据在 ../../04-Stage161-UART-Evidence/20261125T184041610331Z/，原 result.json 与 UART 保留，fam-acceptance.json 区分 FAM init 与网络验收；当前源码已继续修正队列 port_bitmap 大端 u64 的高低字顺序并补充原厂 EQM 三寄存器前置序列，这些后续修改不能归入本次已执行 KO

真实原厂 DT 采样与 hi_dp_probe 的 of_property_read_u64 证明 port_bitmap 为 0x00ff0f0300011ffc；普通队列 dump 与原厂静态表共同确认 24 个端口、192 条普通队列，另外 16 号端口无静态队列、32 号端口 qid/count 为零，辅助队列另行保留，不把它们冒充普通 192 行已初始化

当前正继续验证 EQM 初始化和表读回，随后实现 PIE/PFE、GRO、DMA STOP 与 owner 归还恢复，任务仍未完成

## 前序阶段记录 20261125T143531238212Z：DP/PFE 绑定与延迟采样问题

2026-11-25 新会话 20261125T143531238212Z 完成双件 TFTP/RAM CRC、一次 bootfip、Linux 7.3.5、Debian ARMEL、Bash nonce、八个模块板端 SHA-256 全匹配及 60 秒无复位观察，主机网络和 TFTP 清理完成

新增 hsan-lsw-crg 提供两个标准 CCF 时钟和 peri/LSW 复位控制器，行为来自原厂内核嵌入 initramfs 中的 hi_clk、hi_crg、hi_kreset，LSW hwver=1 为 0x14980024 bit1/bit2 高电平时钟使能，复位为清位断言、置位释放，Linux reset status 已统一为 asserted 语义，新内核启用 CONFIG_RESET_CONTROLLER，八 KO 经官方 LLVM 22.1.8 Kbuild/modpost 与 KCFLAGS=-Werror，checkpatch 零错误零警告

本轮实际装入 RX 描述符修正版及新增 CRG 模块，DP 时钟开启、36MiB queue 与 4MiB+64KiB flow 保留内存几何验证、DP/PFE 窗口映射和 PIE 绑定已观察，启动阻塞位从 0xbf 降为 0xb3，FAM 未 arm，LAN1 仍 DOWN、TX/RX=0、ndo_open=-13，GRO 和 DMA STOP/owner 归还尚未验收

首次诊断脚本在延迟探测完成前采样，原 result.json 保留为 DEBIAN_SHELL_READY_DRIVER_DIAGNOSTIC_INCOMPLETE；启动约 14 秒后 DP/PIE 完成探测，同次补采 final-audit.raw 确认八个模块文件哈希全 OK、八个 Live、LAN1 owner=hsan_net_v300、WDT dev=254:0 且绑定 dw_wdt、实际一次 ndo_open=-13，不把补采伪装成原脚本已通过

证据在 ../../04-Stage161-UART-Evidence/20261125T143531238212Z/，包含原 UART/result、postprobe.raw、final-audit.raw、执行配对与资产身份、reconciled.json；此内核未启用 DEBUG_FS，debugfs 时钟摘要不可用，不据此声称计数或 STOP 状态已证实

原厂 Linux 5.10 只读参考记录在 ../../work/oem-runtime-reference/，确认 LAN1=eth3=port10、普通优先级队列 81..88、FAM 512×8192 与 2048×16384、DP/PFE 时钟各 50MHz、原厂 L3intf 和 GRO 表 allocated=0；原厂实际收发计数不作为新 ARM64 驱动验收，提取模块和参考记录身份见 provenance.json

下一步修正诊断等待延迟探测的时序，验证原厂几何对应的 CPU-owned FAM 地址表，并继续实现 DP/FAM/EQM/PIE、PFE 普通转发、GRO、DMA STOP 与恢复，不移除未满足的硬件门控

上一轮 WDT-only 证据仍保存在 20261125T124939706780Z，identity/state/timeout 缺失由 CONFIG_WATCHDOG_SYSFS 未启用解释，WDT 注册和 60 秒无复位均不能替代实际硬件计数、保活与 DMA STOP 证据

## 最近实板测试：脚本注意事项与真实证据（2026-11-25，现行结论）

本节基于真实 `result.json` 与对应 `uart.raw`、`uart-events.jsonl`、主机网络日志核对，按 **同一载荷配对／同一次会话** 解释，不能用其他轮的 PASS 补足未执行关卡。

| 实板会话（UTC） | 测试组合与结论 | 不能推出 |
|---|---|---|
| `20261125T075340951649Z` | Stage161 WDT 原组合：Linux 7.3.5、Debian ARMEL、Bash nonce 应答、7/7 `Live`、LAN1 owner=1、PHY3 `0x20669a22`、`ndo_open=-13`；记录 `OBSERVED_NO_RESET_60S`。 | WDT 设备为 `NOT_FOUND`，60 秒未复位不能证明硬件 WDT 已注册；无真实 LAN1 收发。 |
| `20261125T082450273638Z` | Stage161 原组合：原版 Linux 先识别→正常 reboot→U-Boot `hy`/version→RAM 引导；七模块 Live、Shell nonce PASS、`ndo_open=-13`；完整观察 60 秒无复位。 | 新增的 `reboot` 命令尚未装入此组合；不是新 reboot 载荷测试。 |
| **`20261125T084342787747Z`（最近）** | **加入 ARMEL 静态 reboot 的新 initrd/FIP**：冷启动接管、双件 RAM CRC、一次 bootfip、Linux/Debian/Bash nonce、七模块 Live、LAN1 owner=1、PHY3 `0x20669a22` 均 PASS；`ndo_open=-13`，接口没有 UP，收发未运行。 | `wdt_device_present=NOT_FOUND`；操作者通过 `Ctrl+]` 提前释放 COM11，`wdt_stability_60s=SKIPPED_BY_OPERATOR_FOR_PUTTY`，不是完成 60 秒测试；**新版 reboot 真机执行仍 NOT_RUN**。 |

**最新已交付测试工具和字节身份：** `TEWA1108V-Stage161-Debian-WDT-LAN1-Test_20261125_164149_D8kP2.zip`，Linux Image SHA-256 `40094da02b687c9d074c7c21e81ab9246655a1c2e6cdf7970a85f779bb31b257`，ARMEL initrd 48,983,956 B / SHA-256 `e0c6a88908fd482e2fc303585e5b3e51fdc3b7030165002749f05695baaf86b1`，FIP 8,099,776 B / SHA-256 `b575a03f8ad6d3ccb3eb90925e4fde645fd66d946e2a818cf967a053b7e3cbb5`。七个 Stage160 KO 原字节不变。此处的硬件 PASS 指板端实际观测，`module_file_hashes=NOT_CONFIRMED` 则保留未证实字段，不把工具的离线 SHA 检验冒充板上文件复核。

**现行研发阻塞：** R1 LSW/DP/PFE 管线的真实就绪条件尚未满足，接口 `ndo_open=-13 (EACCES)` 是 fail-closed 结果；R2 PIE RX4–7 GRO 与 R3 DMA STOP ACK/owner 归还未验收，均不得强制解禁。WDT `/sys/class/watchdog/watchdog0` 未发现，须继续核查有效 DTB、clock/probe 与实际设备状态；旧版曾发生复位不等于新版 WDT 已接管。历史实板、虚拟机、离线编译、当前同版实板不可互相替代。

**使用者重要注意：** 最近几轮的真正失败曾是串口状态机误判或宿主观察器提前退出，并非内核启动必然失败。新运行器初始先探测 `telnetadmin`，但如果发现已有 `TEWA160#` 或 `bash: telnetadmin: command not found`，不得把测试 Debian 判作已登录原版系统，更不得再自动发 `reboot`/`version`；退出并释放 COM11。UART `Password:`、`Login:` 是接收到的设备输出，非脚本伪造。只有本次探测对应的 `Password:` 才能进入认证步骤；不能让周期性 `[gwd]` 日志干扰。Linux 进入 Bash 后，命令 nonce 应答才确认真正可交互；60 秒观察应允许键盘直通，`Ctrl+]` 提前退出必须明确记为 SKIPPED。具体操作规则以《开发与测试规范》第 3、6 节为准。

## Stage161 Debian ARMEL reboot 命令与串口误判修正（20261125_164130 UTC+8，离线）

已经使用 LLVM 22.1.8 真实编译独立静态 ELF32 ARM EABI5 的 `reboot`（1,372 字节，SHA-256 `a817516637411c95b653cf700a58c06aaecc6e95c00ccca8553968900be5ea07`）。新版 Debian initramfs 增加 `/usr/sbin/reboot` 和 `/usr/bin/reboot` 相对符号链接；`reboot` 命令先执行 ARM `sync` syscall 36，再以用户 root/CAP_SYS_BOOT 调用 Linux `sys_reboot` 88 与 `LINUX_REBOOT_CMD_RESTART`。不依赖 systemd 或 PID1 重启服务；不写 NAND / 不修改原版 WDT / DMA 门禁。**真实实板 reboot 尚未执行或验收**。

新 initrd 48983956 B / SHA-256 `e0c6a88908fd482e2fc303585e5b3e51fdc3b7030165002749f05695baaf86b1` / CRC `6538d8da`，FIP 8099776 B / SHA-256 `b575a03f8ad6d3ccb3eb90925e4fde645fd66d946e2a818cf967a053b7e3cbb5` / CRC `ffe61192`。只新增两个 CPIO 条目及重配存储 DTB `/chosen/linux,initrd-end`；Linux 7.3.5 Image、BL31、stub、overlay、七个 KO 和既有 ARMEL PID1 原字节不变。这个新配对的 Linux 启动和 reboot 尚未上机，之前成功的 Stage161 7 Live / Bash nonce / 60 秒无复位是**旧配对**的实板证据，不能借用。WDT 节点仍未在之前实板观测中发现，`ndo_open=-13`，网络数据面继续 fail-closed。

旧脚本会把 `bash: telnetadmin: command not found` 误作原版系统已登录，发 `reboot` 并在没有 reboot binary 的旧测试 Debian 中失败。修复后默认仍先盲探 `telnetadmin`，但只要出现该 Bash 错误或明确 `TEWA160#`，即判定测试 Debian 已运行，**不再自动发 reboot/version，不再无限等待 U-Boot**，写明已有系统并释放 COM11 供 PuTTY。真正的原版系统 `Password:` 登录、普通 reboot、U-Boot version、启动 `hy` 保持原流程。旧版运行中的 RAM 内容不会因为主机新 ZIP 自动更新，必须新一轮 RAM 启动后才能使用 reboot。单一完整测试入口、Host ANSI 颜色与简化 `logs/` 继续保留。

## Stage161 Windows Shell 交互与日志层级修正（本轮离线完成）

用户报告 Stage161 已完成真正的 `TEWA160_INTERACTIVE_<nonce>` 回显后，Windows 测试工具仍阻塞 COM11 60 秒、不能输入，且日志原路径为 `work/logs/<UTC-session>/`。本轮只修正 Windows 运行器：测试 Debian Shell 的 nonce 验证通过后即开放本地 pwsh 键盘直通；保留 60 秒无复位观察，用户可用 `Ctrl+]` 即刻释放串口并明确标记本次未完成 60 秒验证；自动结束时亦释放 COM11。新日志直接放在 `logs/`，旧日志自动归档 `logs-history/<session>/`。不改 Image、DTB、initrd、七个 KO 或 LAN1 数据面门控。**新增键盘直通和快捷键尚未实板运行，不能称为真机验证。**

## Stage161 当前实板成果与原版系统自动接管（2026-11-25）

本轮最新上传的会话 `20261125T075340951649Z`（完成 UTC `07:55:20`）证实：现有 Stage161 Image+Debian ARMEL initrd+FIP 双件 TFTP/CRC PASS；一次 OEM RAM `bootfip` 已发送，Linux 7.3.5 / Debian ARMEL PID1 运行、Bash nonce 命令应答 PASS，7/7 Stage160 KO Live、PHY3 ID `0x20669a22`、LAN1 owner=1、`ndo_open=-13`（原 fail-closed）得到实际板端结果。60 秒观察中没有发现复位，但 **`wdt_device_present=NOT_FOUND`、模块文件 SHA 未在本次报告中确认、LAN1 数据面仍被门控**；不得把 60 秒无复位推导为 WDT 注册或网络收发成功。

接到用户新增三种串口状态要求后，Windows `windows_host.py` 和唯一入口已增加：启动中按原 `hy`；U-Boot Shell 按原 `version`；原版 Linux 自动登录后单次普通 `reboot`，随即继续原 U-Boot/version/端点/TFTP/CRC/bootfip 链。用户已提供认证口令，密码 TX 事件脱敏；`telnetadmin` 返回自身命令错误时改发 reboot，带前缀或仍见原版 `$` 时重试；U-Boot 明确不支持 reboot 则改 version 验证。新增分支经离线分包回放 PASS，但 **尚未实板验证自动原版登录和 reboot**。不再要求用户在原版系统状态手动断电启动测试；不改变 Linux Image、DTB、initrd、七模块或门禁字节。

## Stage161 WDT 本轮独立新成果（20261125_153747 UTC+8）

**已完成真实构建，不是旧载荷改名。** 从已保存的 `linux-7.3.5.tar.xz` 和 LLVM 22.1.8 四分卷恢复构建，真实 ARM64 `Image` 编译返回码 0；新 `Image` 为 7,931,912 B，SHA256 `40094da02b687c9d074c7c21e81ab9246655a1c2e6cdf7970a85f779bb31b257`。`llvm-nm` 核实 vmlinux 链接 `dw_wdt_drv_probe`、`watchdog_ping_work`。当前内核配置为 `CONFIG_WATCHDOG=y`、`CONFIG_DW_WATCHDOG=y`、`CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED=y`、`CONFIG_WATCHDOG_OPEN_TIMEOUT=0`、`CONFIG_WATCHDOG_NOWAYOUT=y`，原来 Stage160 七个模块未变字节。

- 新 Debian ARMEL initrd：48,983,301 B / CRC `63a88831` / SHA `747bd922e619fccd2a3b1c86fe10364c4575c5e9e753f112d98288454cf008fb`；只修改诊断脚本，记录 `/sys/class/watchdog/watchdog0` 状态，修正 `/proc/modules` 引用计数为 `-` 时的 Live 统计。Bash 和原始 ARMEL PID1 均保留。
- 新 OEM RAM FIP：8,099,776 B / CRC `8129a69c` / SHA `5109c0805934932647f6a1bfb7a5f06e7b216dcc22291fa44e1e10aa36a7dea7`。BL31、456 字节入口 stub 和 OEM overlay 与 2026-11-25 06:50 UTC 实板已运行的 Stage160 组合逐字节一致。FIP 中 WDT 节点从两个时钟改为单 `tclk`，仅移除无现行 Linux 提供者的 `pclk`、可选 pretimeout IRQ；其它设备节点不变，`/chosen/linux,initrd-end` 与新 CPIO 精确配对。**存在 OEM 时钟门控依旧生效的实板未验证假设；实板有效 DTB 也未完全核验。**
- 测试入口仍为**一个完整 Linux + LAN1 单次 RAM-only 流程**，Shell 真实 Bash 命令返回后再被动观察 60 秒硬件是否复位；没有离线/接续/TFTP-only 选择分支。源码和新 Image 留在本包 `源码/WDT接管/` 与 `构建产物/WDT候选/`，完整 49MB initrd + FIP 在独立测试包中，未混入旧已发布 Stage160 原件。
- **证据边界：本轮仅离线完整 ARM64 构建、二进制格式、校验及配对 PASS；Stage161 当前板上 Shell 与七模块已经有真实证据、60 秒无复位；WDT probe 未发现设备、注册和长稳仍未验证，不能报硬件 PASS。** Stage160 旧实板七 Live、PHY 和复位事实继续单独保留。LAN1 真实收发 / GRO / DMA STOP ACK 均继续阻塞，安全门控不变。

# 当前状态与验收

## 1. 当前基线与证据等级

| 子系统 | 当前版本 | 已完成 | 尚未完成 |
|---|---|---|---|
| U-Boot | R134 | 累计纯源码、页表间隙与 BD/GD 布局；干净归档复建、161 宿主检查、152 受保护 AArch64 参考对象；WFE 未变 | 同修订实板受支持入口、独立恢复、DDR/栈/缓存交接；实板启动 NOT_RUN，不可刷写 |
| LAN1 | Stage160，33 累计补丁，7 ARM64 KO | 真实 Linux 7.3.5 / LLVM22.1.8 Kbuild；906 项内部源码清单核验；Image 与七 KO 对应 Stage161、准入 pins；同版实板七 Live、LAN1 owner、PHY3 link 已证实 | WDT 注册仍未确认、长稳 Shell、可信绑定许可闭环、DMA 收发、GRO、STOP ACK |
| ARMEL | Debian 13 armel，首轮绑定链 | Stage160 精确内核通用 QEMU 的 report-only PID1；独立 Stage161 官方 SysV 七模块 Live、0 目标绑定；19 C + 1 汇编 ET_REL；旧 report-only ET_EXEC 静态链接 | Stage161 当前板 Debian ARMEL PID1、Bash nonce 应答及 60s 无复位已实测；长期稳定与可信 receiver 仍缺 |

本次整理交接结构与资料空间，重建 LAN1 最终源码、提取 U-Boot 当前板级移植层，ARMEL 仅按用途改名及修复 include 路径。未改变驱动或既有 Image/KO/DTB/FIP/CPIO，未新增实板启动许可或访问板卡。

## 2. 最新实板证据（区分 report-only、历史与完整 Debian 组合）

- **Stage160 观察组合**：2026-11-24 11:50 UTC，COM11，865 B initrd CRC `9bb15f14`，8,099,808 B carrier CRC `e24a5ca1`，双件同次 TFTP 与 RAM CRC、清理 PASS，`boot_sent=false`。观察 initrd 只含 5 个非 trailer 条目、1420 B ARMEL `/init`，无 KO 或 Debian shell；不能用于绑定验收。当前载体仍含历史实验 BL31（非 stock）；随后 04:20 UTC 已观察到正常 OEM load success 与当前 Linux/PID1 启动，最终有效 DTB 及生产认证仍未证实。
- **Stage74-C / Stage73**：2026-11-24 14:24–14:25 UTC，Windows COM11，静默 `version` 握手、TFTPy 0.8.7、双文件长度/CRC、单次 `bootfip`、Linux 7.3.5/ARMEL shell、7 Live、PHY3、LAN1 owner、一次 `ndo_open` 拒绝、清理均通过。数字输出有已知越界，直接 errno=null；未测试 DMA 或联网。冷启动 `hy` 路径未获得接受成功的实板证据。详见本文件验收矩阵与 `资料/证据.json`。

2026-11-25 本轮测试交付验证：会话 `20261125T015507263290Z` 因旧解析器先于 version 验证把倒计时归零判失败，未 TFTP。用户随后在 095918 修正版会话 `20261125T015949506169Z` 报告 `PASS_CURRENT_PAIR_RAM_CRC_ONLY`，控制台直接显示 carrier 8,099,808 B、CRC `e24a5ca1`，及两件通过的运行器最终状态、未启动/未加载模块/无 Linux 流量；本次只收到控制台节选，不伪造独立 initrd 字段、完整 UART 或 result.json。无需再次单独测试同一载荷。

用户最终修订规则：发送 version 后，残留字符拼接造成的原厂 Unknown command（如 xxxversion）或返回 U-Boot 版本信息，任一即成功；其它输出始终等待，无身份识别超时，不要求回显/GNU ld/prompt 或第二次探测。已同步修改现行规范、Windows UART 库和测试入口；这次新规则是 095918 实板成功后的独立修订；113413 包已实板验证 `VERSION_REPLY` 分支，另一 `Unknown command` 分支仍未实板验证；后续 05:58 UTC 实板已确认冷启动接管。载荷字节未变，pwsh 解压使用明确目录和 LiteralPath。

2026-11-25 03:58:57–03:59:03 UTC，会话 `20261125T035857080845Z`：收到完整 result.json、原始 UART 和主机网络日志；实际测试包与现行交接的 Windows 通信库及两份载荷逐字节匹配，完整性/校验值复核 PASS。`VERSION_REPLY`、双件 TFTP、长度与板端 CRC、本机回环及网络清理均 PASS；最终串口为 OEM U-Boot prompt，`boot_sent=false`、`boot_tx_complete=false`，Linux/PID1、模块绑定、收发、GRO、DMA drain 均 NOT_RUN。此次是重复传输测试，不能提升 Linux 启动证据。禁止再独立重复 UART/TFTP；下一实板关卡必须是当前组合真正进入 Linux，许可与载体准入前置满足后，在同次启动采集 PID1/最终 DTB 及获准的绑定诊断。现有 865 B report-only 观察件没有模块或 Shell，且实验载体 OEM 接受及恢复未证实，不能只增加 bootfip 就作为已就绪的集成测试。


**2026-11-25 04:20 UTC 最新启动证据：** 用户提供 result.json、uart.raw、host-network.log；会话 `20261125T042029134616Z` 单次 bootfip 成功，Linux 7.3.5、ARMEL `/init` 标记 `TEWA_STAGE0 PARK_NO_TRUSTED_HANDOFF` 实际出现。未包含交互 Shell、七 KO 未加载；不能冒充完整 Debian 或联网。

**完整 Debian 诊断已离线生成**：Debian 13 ARMEL Bash、当前七 KO、诊断程序、配对 FIP；`123340` 实板尝试被操作者中止，`PHASE=OEM_ENDPOINTS`、`BOOT_TX_COMPLETE=False`、`SHELL=NOT_CONFIRMED`。日志显示未接管倒计时、原厂 NAND 引导，完整 raw 未提供；不能确定是否存在开机横幅误判的实际过程。现行修正保持 version 和端点查询同一监听循环，区分盲探测后的启动横幅与实际命令响应。新包 `TEWA1108V-Stage160-Debian-LAN1-Test_20261125_123713_D8kP2.zip` SHA-256 `ac5c3d0b43a386afb43bf0f2d18141dcb653ce40660307c106e4950c60bc367a`；五种模拟回放及实际解包离线校验通过，未重新编译驱动或改动 payload。当前完整 Debian Shell、绑定、数据面实板仍未确认。该旧包后来已完成完整 Debian RAM 传输与单次 bootfip 发送，见下述最新证据；不再重复观察件。

**2026-11-25 05:58 UTC 完整 Debian 当前实板证据：** 会话 `20261125T055846099106Z`，实际冷启动 `hy`、`VERSION_REPLY`、端点、双件 RAM CRC 与清理 PASS；完整 initrd 48,983,100 B / CRC `697de6ed`，FIP 8,099,808 B / CRC `748de3d1`。`bootfip` 完整发送一次；观察器错误把 `boot from kernela and update reg to 11` 判作 NAND 回退，提前关闭 UART。对照 04:20 UTC 已成功 RAM 启动原始 UART，同一句及同 img_size 确实出现。故旧 JSON 的回退标志是解析器误报，真实后续 Linux/Shell/模块/门禁及 NAND 来源仍未观察，不推断失败或通过。修正版 `TEWA1108V-Stage160-Debian-LAN1-Test_20261125_140440_D8kP2.zip` SHA-256 `778d0b2e8a1e36bfc45abbaf7c21b5730111603779da8af00373015d1cde2b43`，21 项包内资产核验、两段真实日志回放、三种模拟场景及解包离线入口通过，载荷与通信库不变。旧版曾提供只读接续查看；该入口已废弃。旧 ioctl 实际 errno 仍缺，不补造。详见 `资料/证据.json` 的 `完整Debian诊断与启动观察修正`。

**2026-11-25 06:46 UTC 用户日志与唯一入口修正：** 此次调用的是已废弃的只读接续入口，日志出现 `ATTACH_ONLY`、设备返回 `Login:`、后续原厂 NAND 启动及 `BOOT_TX_COMPLETE=False`，不是完整测试路径的 hy 接管失败。依据用户明确要求删除所有测试模式选择，只保留完整 Linux + Debian ARMEL + Stage160 LAN1 流程。新包 `TEWA1108V-Stage160-Debian-LAN1-Test_20261125_145433_D8kP2.zip` SHA-256 `7f131bbde0b101fd46f722e852354b6e8c31c9bfdd41df9d2da479d1dad98ce1`，21 项资产与干净解压离线校验通过，旧三种模式参数及观察时间参数均不再接受，COM/网卡端点可选。两件 payload、source、通信库原字节不变；实板运行 NOT_RUN。前一次完整 Debian 启动、Shell、模块、门禁仍未观察，不提升验收等级。

**2026-11-25 06:50–06:54 UTC 当前完整 Debian 实板：** 会话 `20261125T065045489785Z` 实际使用 `TEWA1108V-Stage160-Debian-LAN1-Test_20261125_140440_D8kP2.zip`（不是 145433 单入口包）。双件长度/CRC、单次 OEM RAM 启动、Linux 7.3.5、Debian 13.7 ARMEL PID1、七个 KO 文件 SHA 检查、7/7 Live、LAN1 owner `hsan_net_v300`、MDIO0 PHY3 `0x20669a22` / BMSR `0x796d` link/aneg 已实测。DP/PFE=-19，无 DMA 启动，接口 open 未执行，errno=null；不宣称网络可用。实际 FDT SHA `2672ec4590f6475f23e409aeb5d31e199c9148dcff8698b02f854c024cac43bd` 已记录，聚合 marker 被 printk 打断，完整 DTB 身份仍未验证。原运行器 ABORTED，guest Live=0 属于错误统计（refcount `-`）；Bash ANSI prompt 已出现，未确认命令应答。随后出现硬件 WDT reset 原厂日志与 Linux 5.10.0。实际复位原因日志已确认，具体接管缺失的因果链待验证。当前内核 `# CONFIG_WATCHDOG is not set`。

本次仅修正 `源码/启动诊断/` 中三个源文件，语法检查和真实 UART 回放 PASS；不生成新可运行包，不复跑旧载荷。后续标准 Linux watchdog 驱动、时钟/reset 和 early handover 必须完整实现并核验；新内核、initrd 和配对未生成。

## 3. 当前实际阻塞与开发顺序

| 优先 | 位置 | 需要完成的实际实现/证据 |
|---|---|---|
| P0 WDT | Linux 7.3.5 watchdog / clock / reset | 现配置未启用 watchdog；实板已报告硬件 WDT 复位。恢复精确源码/LLVM22.1.8，核实 DW WDT 正常接管、启动中时钟/reset 和持续保活，重新构建并配对；不直接写寄存器或关闭看门狗。 |
| P0 R1 | Stage160 `hsan-lsw-csr.c:2336`、`hsan-lsw-pfe.c:550–572`、`hsan-net-v300.c:2500–2542,2645–2680` | `pipeline_operational()` 固定 false、L2/L3 forwarding blocker、GRO/LSW readiness 导致 open 在队列分配前拒绝。实现普通 PIE–GEMAC 路径，物理 L2/L3 lookup/转发表与 ownership、FAM/EQM/PIE 初始化和真实完成条件；不能把全部 NAT/offload 假定为基本 Ethernet 必需。 |
| P0 R2 | PIE RX4–7 与四组共享 IRQ | GRO 独立描述符、聚合元数据/交付、IRQ ACK、idle 和所有权；或有物理证据的受控普通 RX profile。 |
| P0 R3 | FAM/EQM、DMA STOP/RESET | 真硬件 STOP ACK、drain、owner 归还；INIT ACK/软件栅栏/静止游标不够。未证实前不得 unmap/热卸载/反复 down-up。 |
| 并行 | `源码/ARMEL绑定/` | 两个缺失头文件已重建；19 单元旧 ET_REL 与静态 report-only ET_EXEC、20 单元新绑定事务 ET_REL 均有记录。默认入口不执行绑定；接通可信 receiver 和精确 7 pins，获得同版载体/最终 DTB/许可，再生成真正绑定 initrd。 |

优先做实现与同版验证，不再要求单独重复串口、TFTP 或历史 report-only 启动。准备完毕后一次集成实板测试自动完成必要前置，并逐项记录同次证据。当前 Stage160 保持 fail-closed，流量不合格，不绕开安全门控。

## 4. 历史实板的准确验收结果（Stage74-C / Stage73）

2026-11-24 14:24:40–14:25:07 UTC，Windows COM11、同一 RAM 会话，`PASS_HISTORICAL_7_LIVE_FAIL_CLOSED_NO_ERRNO`。

| 项目 | 实测 |
|---|---|
| UART | 静默 shell `version` 完整返回，OEM 2022.07 / 正确端点；没有证明冷启动 `hy` 被接受 |
| TFTP | TFTPy 0.8.7，真实命名只读文件，双件本机回环与清理 PASS，实际 UDP69 bind PASS |
| rootfs | 48,966,937 B，CRC32 `b2650323`；SHA `bf0d9febfd48af1e5d6b06438169b528f433708a129948d03f4e7c9fc716fe39` |
| FIP | 8,099,808 B，CRC32 `cadabd43`；SHA `71f1ffa456408d6542f877626d4324a82f4b7cda3a7f4b1a0cf078d65bafe920` |
| boot | 一次 `bootfip`，Linux 7.3.5 / LLVM22.1.8，ARMEL PID1 / Shell |
| 驱动 | 7/7 `/proc/modules Live`，MDIO0 PHY3 `0x20669a22`，BMSR `0x796d`、link/aneg；GEMAC port10 GMII，PIE 4 IRQ、TX 4096、RX 1024，Stage73 仅 RX0 软件实现 |
| 门禁 | LAN1 owner，确实执行一次 ioctl，fail-closed 拒绝；数字损坏，直接 errno=null（源码推断 -95，不是实测） |
| cleanup | TFTP 与 Windows 网络 PASS，未删已有 IP；历史日志中文网卡名解码错显 |
| 数据面 | LAN1 TX/RX、UDP、DHCP、GRO、DMA STOP ACK 全部 NOT_RUN |

原始 UART/JSON/网络日志 SHA 和完整 7 模块名见 `资料/证据.json` 的 `历史实板`。不重复保存旧原始日志和每次脚本错误报告。此次结论只适用于历史组合，不能用来宣称 Stage160 启动或绑定。

## 5. 当前 LAN1 验收矩阵

| 关卡 | 当前证据 | 状态 |
|---|---|---|
| T0–T3 | 当前源码归档 906 项核验、33 补丁、7 ARM64 KO；真实 Kbuild；ARMEL 独立 ET_REL/旧 report-only ET_EXEC；精确 Image | PASS 已核验范围，未重编 |
| T4 | LSW/PFE/GRO/STOP 当前门禁保持；R1/R2/R3 缺实现及实板证据 | BLOCKED 正常数据通路 |
| T5 | 2026-11-24 11:50 UTC 完整记录 PASS；2026-11-25 01:59 UTC 095918 修正版控制台 PASS；03:59 UTC 113413 完整日志确认现行 VERSION_REPLY、双文件长度/CRC 与清理 PASS，05:58 UTC 完整 Debian 双件 RAM CRC PASS、冷启动接管实板 PASS；另一识别分支未实板验 | PASS_CURRENT_PAIR_RAM_CRC_ONLY |
| T6a | 当前完整 Debian 同版实板 Linux 7.3.5、ARMEL PID1、Bash prompt；随后硬件 watchdog reset | PASS Stage161 实板 Bash nonce 应答与 60s 无复位；长期稳定/WDT 注册待证 |
| T6b | 当前七 KO SHA 检查、7 Live、LAN1 owner、PHY3 link；可信 receiver 未测试，open 未执行 | PASS Stage161 7 Live/owner/PHY3；实板 ndo_open=-13 继续 fail-closed；可信准入与数据面 BLOCKED |
| T7 | 普通 LAN1 真实转发、ARP、双向 UDP、DHCP | NOT_RUN；不放开门禁 |
| T8 | GRO、物理 STOP ACK/drain、DMA owner/恢复、长稳 | NOT_RUN；阻断释放/热解绑 |

Stage160 观察件 SHA：initrd `ca134e62be905eb4446d8adb6382b81dc8f1f8d456d824cbd5f55a326fe01446`（CRC `9bb15f14`），carrier `b61916ee64682895497e43bf2317746d6345c09ce60b9cf0db9d03b38132e3ab`（CRC `e24a5ca1`）。两件原先传输已通过，随后 04:20 UTC 单次 bootfip 并实际观察到 report-only PID1。当前完整证据在 `资料/证据.json` 的 `当前网卡复核`。


## 6. 依赖与验收关闭条件

| 优先级 | ID | 已知缺口 | 完成/上机证据和停止条件 |
|---|---|---|---|
| **P0** | R1 | PFE/IFC/PA/PUB L2/L3 主机转发、port10↔PIE 硬件交接未完成；`pipeline_operational()` 固定 false | OEM 寄存器/表项/lookup 真实激活与 owner/ACK、读回有效、错误隔离、准备 API 真返回；没有证据就保持 false |
| **P0** | R2 | RX4–RX7 GRO 缺 descriptor metadata、聚合/回调/交付；与 RX0–3 共 IRQ | GRO 单独几何和所有权、共享 IRQ ACK/idle/NAPI、skb 处理验证，或证明 4RX/0GRO 真正停止 GRO HW 访问 |
| **P0** | R3 | FAM DMA STOP、RESET、ACK、outstanding drain 不可靠 | 硬件 STOP 位语义、busy→ack 转换、DMA consumer 不再持有 map，超时 fail-closed；才允许 unmap/down/up |
| P0 | R4 | down/up、driver unload/reload、suspend/resume 无 DMA 安全恢复 | 以 R3 为前置；每次反复关启有硬件 owner 返回证据，不允许“试试 rmmod” |
| P0 | R5 | 强制 remove/unbind 与 devm 自动释放风险 | 平台移除不可在 hardware DMA 未停止时释放 ring；记录 quarantine 与主动拒绝 |
| P0 | R6 | L3 两行写入 CPU 同锁非硬件原子，rollback/lookup 未验 | DMA 静止窗口、preimage、表项提交/激活、硬件状态对照及失败回滚 |
| P1 | R7 | FAM/EQM/PIE map/clock/reset/ACP 实际硬件启动链缺闭环 | 分阶段所有权、真实 init ack、queue producer/consumer、错误计数、时钟释放正确；INIT ACK ≠ STOP ACK |
| P1 | R8 | 当前同版普通诊断绑定已实测；可信绑定、稳定运行与许可范围内门禁结果仍缺 | 当前同版一次 UART、7 Live、绑定/owner、无 panic；按独立许可采集 ioctl，未授权不 UP；不能借 Stage73 log |
| P1 | R9 | 真实 TX/RX 收发、GEMAC/PFE MAC 筛选、链路重配、收发统计未知 | 生产/消费游标前进，ARP 双向、Win↔Linux UDP payload/ACK 都通过，异常恢复安全 |
| P1 | R10 | GSO、checksum、VLAN/NAPT/TNL 等卸载特性不全 | 基本收发的必要路径先解决；不支持的高级 offload 关闭并如实报告 |
| P2 | R11 | DHCP、DNS、`apt-get update`、长期运行、稳态与 watchdog | 双向 UDP 与 DHCP DORA、DNS、apt 真验证；10–60min 统计/STOP ACK/恢复；无证据不得长期上线 |



## 7. 可复用的工程依据

- 当前工作源码：`源码/LAN1/` 为全部 33 个累计补丁应用后的 21 个文件，逐项匹配正式最终输出 SHA-256；`源码/U-Boot/` 为 R134 的完整 `upstream-port` 可编译移植层（代码与脚本原字节，未含 161 项独立离线模型套件）；`源码/ARMEL绑定/` 为功能命名的独立代码。完整正式发布 ZIP 仅在资料空间 `/TEWA-1108V/发布原件/`，准确名称与 SHA 在外部资源表。版本证据不因路径改名提升；本次未重编内核和 U-Boot。
- `构建产物/LAN1/` 保存同版 Image 和七个 KO；`构建产物/启动观察件/` 保存已实际传输并观察到 Linux/PID1 启动的 report-only CPIO 与实验载体，其配对身份 JSON 字节不变。两者不是绑定载荷。Image SHA `d55149444eb63e86f1ff94b8a1cc2a90aece3a3d47d269958b13fcf8dcd67eb2`；存储 DTB SHA `fcbd81484050d793d2b48c58986f2974e8547b58887220a42460848e958458c2`；最终有效 DTB 未证实。
- 基础 Debian 13 armel CPIO 曾重建得到 48,881,149 B / CRC `10a83699` / SHA `8957c7a442d231dbb6c06984621b4e4612d4ef11e41d751262d985be48329199`；不是 48,880,169 B 串口 CPIO 或 865 B 当前观察件。禁止混用。
- 历史通用虚拟机：确切 Linux Image 的报告程序确为 PID1，仅报告/等待约 4 秒，无模块及目标网卡；独立官方 SysV 同版七模块 Live、五驱动注册、零绑定。ARMEL ABI、密封 FD 和 ET_REL 验证不代表实际接收者或实板启动。实际代码编译使用 Clang/LLD 17.0.0，不冒称内核使用的 22.1.8。
- 早期真机 TX 提交 7、consumer 未前进、RX=0，UDP/DHCP 不通；说明提交计数、link、Live 不是数据面完成。该历史结论已经转化为规范的验收规则，不再重复启动旧镜像。

## 8. 资料空间与源码维护结果

Stage74–160 主版本号独立文件覆盖 84 个阶段，共 172 个；91、110、157 未发现独立主版本文件，不意味着累计补丁或成果缺失。172 个文件全部已移出资料空间根目录，原件字节、名称和身份不变：正式原件 7、历史源码发布 65、技术证据 34、旧测试与交接 52、已核验重复上传 14。另将当前 U-Boot R134 原件放入同一正式原件目录。Stage161、Stage165 文件名中的 Stage160 仅为引用，未移动。14 个别名经实际 SHA-256 核对，现行资源引用只保留对应正式或原名文件；别名集中隔离而非删除。

**下一步：** 优先修复 WDT 接管并重建完整配对，稳定 Shell 后完成 R1 普通数据面实现和同版绑定链，R2 GRO 与 R3 DMA STOP ACK 独立闭环；仅在前置及许可满足后做当前版本一次集成测试，不复跑已经证明的历史主机流程。
