param(
    [string]$PythonPath = '.venv/Scripts/python.exe',
    [string]$Adapter = '以太网',
    [string]$SerialPort = 'COM11'
)
$ErrorActionPreference = 'Stop'
$interfaceIndex = (Get-NetAdapter -Name $Adapter).ifIndex
$rule = 'TEWA-DORA-' + [guid]::NewGuid().ToString('N')
$dnsRule = $rule + '-DNS'
try {
    New-NetFirewallRule -Name $rule -DisplayName $rule -Direction Inbound -Action Allow -Protocol UDP -LocalPort 67 -RemotePort 68 -InterfaceAlias $Adapter -Profile Any | Out-Null
    New-NetFirewallRule -Name $dnsRule -DisplayName $dnsRule -Direction Inbound -Action Allow -Protocol UDP -LocalPort 53 -RemoteAddress 192.168.0.1 -InterfaceAlias $Adapter -Profile Any | Out-Null
    & $PythonPath -B (Join-Path $PSScriptRoot 'test-lan-dhcp-dns.py') --interface-index $interfaceIndex --port $SerialPort
    if ($LASTEXITCODE -ne 0) { throw 'DHCP_DNS_VALIDATION_FAILED' }
} finally {
    Remove-NetFirewallRule -Name $rule,$dnsRule -ErrorAction SilentlyContinue
}
