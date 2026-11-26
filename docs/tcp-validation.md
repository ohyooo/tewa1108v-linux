# TCP 验证

验收结果见 [roadmap](lan1-progress.md)，详细记录保留本地

复现工具为 `tools/ram-boot/test-lan-tcp.ps1` 及同目录 Python 脚本，使用已有 pyserial 环境和管理员 PowerShell，参数包括 `-Port`、`-Seconds` 和 `-PythonPath`

工具按需建立并清理临时 TCP 入站规则，板端临时文件也在结束时清理
