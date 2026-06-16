param(
    [string]$Target
)

$ErrorActionPreference = "Stop"
$port = 40771
if ([string]::IsNullOrWhiteSpace($Target)) {
    $Target = Read-Host "Enter the other PC IPv4 address"
}

$address = [System.Net.IPAddress]::Parse($Target)
$endpoint = [System.Net.IPEndPoint]::new($address, $port)
$client = [System.Net.Sockets.UdpClient]::new()
$client.Client.ReceiveTimeout = 1500

Write-Host "Sending UDP test packets to $Target`:$port..."
$ok = $false
try {
    for ($i = 1; $i -le 10; $i++) {
        $payload = [System.Text.Encoding]::UTF8.GetBytes("tvo ping $i from $env:COMPUTERNAME")
        [void]$client.Send($payload, $payload.Length, $endpoint)
        try {
            $remote = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
            $reply = $client.Receive([ref]$remote)
            $text = [System.Text.Encoding]::UTF8.GetString($reply)
            Write-Host "Reply from $($remote.Address):$($remote.Port): $text"
            $ok = $true
        } catch [System.Net.Sockets.SocketException] {
            Write-Host "No reply for packet $i"
        }
        Start-Sleep -Milliseconds 500
    }
} finally {
    $client.Close()
}

if ($ok) {
    Write-Host "UDP direct test PASSED."
} else {
    Write-Host "UDP direct test FAILED. Firewall, router, VPN, or provider NAT is blocking UDP $port."
}
Read-Host "Press Enter to close"
