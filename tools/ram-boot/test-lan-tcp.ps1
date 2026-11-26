param([int]$Port = 18080, [int]$Seconds = 0, [string]$PythonPath = '.venv/Scripts/python.exe')
$ErrorActionPreference = 'Stop'
$rule = 'TEWA-TCP-' + [guid]::NewGuid().ToString('N')
try {
    New-NetFirewallRule -Name $rule -DisplayName $rule -Direction Inbound -Action Allow -Protocol TCP -LocalPort $Port -LocalAddress 192.168.0.10 -RemoteAddress 192.168.0.1 -InterfaceAlias '以太网' -Profile Any | Out-Null
    & $PythonPath -B (Join-Path $PSScriptRoot 'test-lan-tcp.py') --port $Port --seconds $Seconds
    if ($LASTEXITCODE -ne 0) { throw 'TCP_ROUNDTRIP_FAILED' }
} finally {
    Remove-NetFirewallRule -Name $rule -ErrorAction SilentlyContinue
    'TCP_FIREWALL_CLEANUP_DONE'
}
