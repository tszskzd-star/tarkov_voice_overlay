$ErrorActionPreference = "Stop"
$port = 40771
$client = [System.Net.Sockets.UdpClient]::new($port)
$client.Client.ReceiveTimeout = 1000
$deadline = (Get-Date).AddSeconds(60)
Write-Host "Listening for UDP on port $port for 60 seconds..."
Write-Host "Keep this window open and run test_direct_udp_send.cmd on the other PC."
try {
    while ((Get-Date) -lt $deadline) {
        try {
            $remote = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
            $bytes = $client.Receive([ref]$remote)
            $text = [System.Text.Encoding]::UTF8.GetString($bytes)
            Write-Host "Received '$text' from $($remote.Address):$($remote.Port)"
            $reply = [System.Text.Encoding]::UTF8.GetBytes("pong from $env:COMPUTERNAME")
            [void]$client.Send($reply, $reply.Length, $remote)
        } catch [System.Net.Sockets.SocketException] {
            if ($_.Exception.SocketErrorCode -ne [System.Net.Sockets.SocketError]::TimedOut) {
                Write-Host "Socket error: $($_.Exception.SocketErrorCode)"
            }
        }
    }
} finally {
    $client.Close()
}
Write-Host "Done."
Read-Host "Press Enter to close"
