param([string]$PythonPath = '.venv/Scripts/python.exe')
$ErrorActionPreference = 'Stop'
$rule = 'TEWA-UDP-' + [guid]::NewGuid().ToString('N')
try {
    New-NetFirewallRule -Name $rule -DisplayName $rule -Direction Inbound -Action Allow -Protocol UDP -LocalPort 18082 -LocalAddress 192.168.0.10 -RemoteAddress 192.168.0.1 -InterfaceAlias '以太网' -Profile Any | Out-Null
    & $PythonPath -B (Join-Path $PSScriptRoot 'test-lan-udp.py')
    if ($LASTEXITCODE -ne 0) { throw 'UDP_ROUNDTRIP_FAILED' }
} finally {
    Remove-NetFirewallRule -Name $rule -ErrorAction SilentlyContinue
}
