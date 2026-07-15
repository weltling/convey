#Requires -Version 5
# Integration tests for convey.
# Drives the real convey.exe over loopback and checks byte round-trips.
[CmdletBinding()]
param(
    [string]$Convey = (Join-Path $PSScriptRoot '..' | Join-Path -ChildPath 'convey.exe')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Convey)) {
    Write-Host "convey.exe not found at '$Convey'; build it first."
    exit 2
}
$Convey = (Resolve-Path $Convey).Path

#region Helpers
$script:failures = 0
function Assert-Equal($expected, $actual, $name) {
    if ($expected -ceq $actual) {
        Write-Host "PASS: $name"
    } else {
        Write-Host "FAIL: $name"
        Write-Host "  expected: '$expected'"
        Write-Host "  actual:   '$actual'"
        $script:failures++
    }
}

function Get-FreePort {
    $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $l.Start()
    try { return $l.Server.LocalEndPoint.Port } finally { $l.Stop() }
}

function Stop-Proc($p) {
    if ($p -and -not $p.HasExited) {
        $p | Stop-Process -Force -ErrorAction SilentlyContinue
    }
}

function Read-Text($stream, $count, $ms) {
    $buf = New-Object byte[] $count
    $task = $stream.ReadAsync($buf, 0, $count)
    if (-not $task.Wait($ms)) { return $null }
    return [System.Text.Encoding]::ASCII.GetString($buf, 0, $task.Result)
}
#endregion

#region TCP transport
function Test-TcpListenRoundTrip {
    $port = Get-FreePort
    $toSocket = "from-stdin-" + ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $toStdout = "from-socket-" + ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $inFile = [System.IO.Path]::GetTempFileName()
    $outFile = [System.IO.Path]::GetTempFileName()
    [System.IO.File]::WriteAllText($inFile, $toSocket)

    $p = Start-Process -FilePath $Convey -ArgumentList "tcp-listen:$port", "--no-xterm" `
        -RedirectStandardInput $inFile -RedirectStandardOutput $outFile -PassThru -NoNewWindow
    try {
        Start-Sleep -Milliseconds 600
        $client = [System.Net.Sockets.TcpClient]::new()
        $client.Connect('127.0.0.1', $port)
        $stream = $client.GetStream()

        Start-Sleep -Milliseconds 400
        $got = Read-Text $stream 256 3000
        Assert-Equal $toSocket $got 'tcp-listen: stdin -> socket'

        $bytes = [System.Text.Encoding]::ASCII.GetBytes($toStdout)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        Start-Sleep -Milliseconds 500
        $client.Close()
    } finally {
        Stop-Proc $p
    }

    Start-Sleep -Milliseconds 200
    $out = [System.IO.File]::ReadAllText($outFile)
    Assert-Equal $toStdout $out.Trim() 'tcp-listen: socket -> stdout'
    Remove-Item $inFile, $outFile -ErrorAction SilentlyContinue
}

function Test-TcpListenIPv6 {
    # The listener is dual-stack, so an IPv6 client must be accepted too.
    $port = Get-FreePort
    $payload = "ipv6-" + ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $inFile = [System.IO.Path]::GetTempFileName()
    $outFile = [System.IO.Path]::GetTempFileName()
    [System.IO.File]::WriteAllText($inFile, $payload)

    $p = Start-Process -FilePath $Convey -ArgumentList "tcp-listen:$port", "--no-xterm" `
        -RedirectStandardInput $inFile -RedirectStandardOutput $outFile -PassThru -NoNewWindow
    try {
        Start-Sleep -Milliseconds 600
        $client = [System.Net.Sockets.TcpClient]::new([System.Net.Sockets.AddressFamily]::InterNetworkV6)
        $client.Connect([System.Net.IPAddress]::IPv6Loopback, $port)
        $stream = $client.GetStream()
        Start-Sleep -Milliseconds 400
        Assert-Equal $payload (Read-Text $stream 256 3000) 'tcp-listen: IPv6 client -> stdin -> socket'
        $client.Close()
    } finally {
        Stop-Proc $p
    }
    Remove-Item $inFile, $outFile -ErrorAction SilentlyContinue
}
#endregion

#region Bridge
function Test-Bridge {
    $port = Get-FreePort
    $pipeName = "conveytest_" + ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $p = Start-Process -FilePath $Convey `
        -ArgumentList "--bridge", "--pipe-server", "\\.\pipe\$pipeName", "tcp-listen:$port" `
        -PassThru -NoNewWindow
    try {
        Start-Sleep -Milliseconds 600
        # Bridge opens the TCP transport (accept) before creating the pipe server.
        $client = [System.Net.Sockets.TcpClient]::new()
        $client.Connect('127.0.0.1', $port)
        $tcp = $client.GetStream()
        Start-Sleep -Milliseconds 400

        $pipe = [System.IO.Pipes.NamedPipeClientStream]::new('.', $pipeName, [System.IO.Pipes.PipeDirection]::InOut)
        $pipe.Connect(3000)

        $a = "tcp-to-pipe"
        $ab = [System.Text.Encoding]::ASCII.GetBytes($a)
        $tcp.Write($ab, 0, $ab.Length); $tcp.Flush()
        Start-Sleep -Milliseconds 300
        Assert-Equal $a (Read-Text $pipe 256 3000) 'bridge: tcp -> pipe'

        $b = "pipe-to-tcp"
        $bb = [System.Text.Encoding]::ASCII.GetBytes($b)
        $pipe.Write($bb, 0, $bb.Length); $pipe.Flush()
        Start-Sleep -Milliseconds 300
        Assert-Equal $b (Read-Text $tcp 256 3000) 'bridge: pipe -> tcp'

        $pipe.Close()
        $client.Close()
    } finally {
        Stop-Proc $p
    }
}
#endregion

#region Reconnect
function Test-TcpClientReconnect {
    # convey connects out with --reconnect; its stdin is an open, idle pipe.
    # Without the stdin-wake fix a worker would block there and the reconnect
    # would never happen, so this exercises patch #1 directly.
    $port = Get-FreePort
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $port)
    $listener.Start()

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Convey
    $psi.Arguments = "tcp:127.0.0.1:$port --reconnect --no-xterm"
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    try {
        $stdout = $proc.StandardOutput.BaseStream

        $t1 = $listener.AcceptTcpClientAsync()
        if (-not $t1.Wait(3000)) { Assert-Equal 'accepted' 'timeout' 'reconnect: first connection'; return }
        $c1 = $t1.Result
        $s1 = $c1.GetStream()
        $m1 = "first-conn"
        $b1 = [System.Text.Encoding]::ASCII.GetBytes($m1)
        $s1.Write($b1, 0, $b1.Length); $s1.Flush()
        Start-Sleep -Milliseconds 300
        Assert-Equal $m1 (Read-Text $stdout 256 3000) 'reconnect: first payload -> stdout'

        # Drop the peer; convey must reconnect and accept a second connection.
        $c1.Close()
        $t2 = $listener.AcceptTcpClientAsync()
        $ok = $t2.Wait(5000)
        Assert-Equal $true $ok 'reconnect: second connection accepted'
        if ($ok) {
            $c2 = $t2.Result
            $s2 = $c2.GetStream()
            $m2 = "second-conn"
            $b2 = [System.Text.Encoding]::ASCII.GetBytes($m2)
            $s2.Write($b2, 0, $b2.Length); $s2.Flush()
            Start-Sleep -Milliseconds 300
            Assert-Equal $m2 (Read-Text $stdout 256 3000) 'reconnect: second payload -> stdout'
            $c2.Close()
        }
    } finally {
        if (-not $proc.HasExited) { $proc.Kill() }
        $listener.Stop()
    }
}
#endregion

#region Console
function Test-ReadOnly {
    # read only suppresses stdin to endpoint but still displays received bytes.
    $port = Get-FreePort
    $recv = "rx-" + ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $inFile = [System.IO.Path]::GetTempFileName()
    $outFile = [System.IO.Path]::GetTempFileName()
    [System.IO.File]::WriteAllText($inFile, "should-not-be-sent")

    $p = Start-Process -FilePath $Convey `
        -ArgumentList "tcp-listen:$port", "--no-xterm", "--read-only" `
        -RedirectStandardInput $inFile -RedirectStandardOutput $outFile -PassThru -NoNewWindow
    try {
        Start-Sleep -Milliseconds 600
        $client = [System.Net.Sockets.TcpClient]::new()
        $client.Connect('127.0.0.1', $port)
        $s = $client.GetStream()
        # Nothing typed should reach the socket here.
        $leaked = Read-Text $s 256 1200
        Assert-Equal '' ([string]$leaked) 'read only stdin not forwarded to endpoint'
        # The receive direction still works.
        $b = [System.Text.Encoding]::ASCII.GetBytes($recv)
        $s.Write($b, 0, $b.Length); $s.Flush()
        Start-Sleep -Milliseconds 500
        $client.Close()
    } finally {
        Stop-Proc $p
    }
    Start-Sleep -Milliseconds 200
    Assert-Equal $recv ([System.IO.File]::ReadAllText($outFile).Trim()) 'read only received bytes still reach stdout'
    Remove-Item $inFile, $outFile -ErrorAction SilentlyContinue
}

function Test-Timestamps {
    # timestamps prefix received lines with a local time stamp
    $port = Get-FreePort
    $inFile = [System.IO.Path]::GetTempFileName()
    $outFile = [System.IO.Path]::GetTempFileName()

    $p = Start-Process -FilePath $Convey `
        -ArgumentList "tcp-listen:$port", "--no-xterm", "--timestamps" `
        -RedirectStandardInput $inFile -RedirectStandardOutput $outFile -PassThru -NoNewWindow
    try {
        Start-Sleep -Milliseconds 600
        $client = [System.Net.Sockets.TcpClient]::new()
        $client.Connect('127.0.0.1', $port)
        $s = $client.GetStream()
        $b = [System.Text.Encoding]::ASCII.GetBytes("hello`n")
        $s.Write($b, 0, $b.Length); $s.Flush()
        Start-Sleep -Milliseconds 600
        $client.Close()
    } finally {
        Stop-Proc $p
    }
    Start-Sleep -Milliseconds 200
    $out = [System.IO.File]::ReadAllText($outFile)
    Assert-Equal $true ($out -match '\[\d\d:\d\d:\d\d\] hello') 'timestamps prefix the received line'
    Remove-Item $inFile, $outFile -ErrorAction SilentlyContinue
}

function Test-Hex {
    # hex renders the received stream as a hex dump
    $port = Get-FreePort
    $inFile = [System.IO.Path]::GetTempFileName()
    $outFile = [System.IO.Path]::GetTempFileName()

    $p = Start-Process -FilePath $Convey `
        -ArgumentList "tcp-listen:$port", "--no-xterm", "--hex" `
        -RedirectStandardInput $inFile -RedirectStandardOutput $outFile -PassThru -NoNewWindow
    try {
        Start-Sleep -Milliseconds 600
        $client = [System.Net.Sockets.TcpClient]::new()
        $client.Connect('127.0.0.1', $port)
        $s = $client.GetStream()
        $b = [System.Text.Encoding]::ASCII.GetBytes("KD")
        $s.Write($b, 0, $b.Length); $s.Flush()
        Start-Sleep -Milliseconds 600
        $client.Close()
    } finally {
        Stop-Proc $p
    }
    Start-Sleep -Milliseconds 200
    $out = [System.IO.File]::ReadAllText($outFile)
    Assert-Equal $true ($out -match '00000000\s+4b 44') 'hex dump shows the offset and bytes'
    Assert-Equal $true ($out -match '\|KD\|') 'hex dump shows the ascii gutter'
    Remove-Item $inFile, $outFile -ErrorAction SilentlyContinue
}
#endregion

#region Logging
function Test-LogRecv {
    # --log-recv tees everything received from the target into a file.
    $port = Get-FreePort
    $payload = "log-" + ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $inFile = [System.IO.Path]::GetTempFileName()
    $logFile = [System.IO.Path]::GetTempFileName()
    $outFile = [System.IO.Path]::GetTempFileName()

    $p = Start-Process -FilePath $Convey `
        -ArgumentList "tcp-listen:$port", "--no-xterm", "--log-recv", $logFile `
        -RedirectStandardInput $inFile -RedirectStandardOutput $outFile -PassThru -NoNewWindow
    try {
        Start-Sleep -Milliseconds 600
        $client = [System.Net.Sockets.TcpClient]::new()
        $client.Connect('127.0.0.1', $port)
        $s = $client.GetStream()
        $b = [System.Text.Encoding]::ASCII.GetBytes($payload)
        $s.Write($b, 0, $b.Length); $s.Flush()
        Start-Sleep -Milliseconds 600
        $client.Close()
    } finally {
        Stop-Proc $p
    }
    Start-Sleep -Milliseconds 200
    Assert-Equal $payload ([System.IO.File]::ReadAllText($logFile).Trim()) '--log-recv: received bytes written to log file'
    Remove-Item $inFile, $logFile, $outFile -ErrorAction SilentlyContinue
}

function Test-LogSend {
    # --log-send tees everything sent to the target into a file.
    $port = Get-FreePort
    $payload = "send-" + ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $inFile = [System.IO.Path]::GetTempFileName()
    $outFile = [System.IO.Path]::GetTempFileName()
    $sendLog = [System.IO.Path]::GetTempFileName()
    [System.IO.File]::WriteAllText($inFile, $payload)

    $p = Start-Process -FilePath $Convey `
        -ArgumentList "tcp-listen:$port", "--no-xterm", "--log-send", $sendLog `
        -RedirectStandardInput $inFile -RedirectStandardOutput $outFile -PassThru -NoNewWindow
    try {
        Start-Sleep -Milliseconds 600
        $client = [System.Net.Sockets.TcpClient]::new()
        $client.Connect('127.0.0.1', $port)
        $s = $client.GetStream()
        Read-Text $s 256 3000 | Out-Null
        Start-Sleep -Milliseconds 400
        $client.Close()
    } finally {
        Stop-Proc $p
    }
    Start-Sleep -Milliseconds 200
    Assert-Equal $payload ([System.IO.File]::ReadAllText($sendLog).Trim()) '--log-send: sent bytes written to log file'
    Remove-Item $inFile, $outFile, $sendLog -ErrorAction SilentlyContinue
}

function Test-LogConflict {
    # Two log options must not name the same file.
    $port = Get-FreePort
    $same = [System.IO.Path]::GetTempFileName()
    $errFile = [System.IO.Path]::GetTempFileName()

    $p = Start-Process -FilePath $Convey `
        -ArgumentList "tcp-listen:$port", "--no-xterm", "--log", $same, "--log-recv", $same `
        -RedirectStandardError $errFile -PassThru -NoNewWindow -Wait
    $err = [System.IO.File]::ReadAllText($errFile)
    Assert-Equal $true ($p.ExitCode -ne 0) '--log conflict: exits with error'
    Assert-Equal $true ($err.Contains('different file')) '--log conflict: reports the clash'
    Remove-Item $same, $errFile -ErrorAction SilentlyContinue
}

function Test-Log {
    # --log marks each block > (sent) or < (received) in one file.
    $port = Get-FreePort
    $sent = "tx-" + ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $recv = "rx-" + ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $inFile = [System.IO.Path]::GetTempFileName()
    $outFile = [System.IO.Path]::GetTempFileName()
    $bothLog = [System.IO.Path]::GetTempFileName()
    [System.IO.File]::WriteAllText($inFile, $sent)

    $p = Start-Process -FilePath $Convey `
        -ArgumentList "tcp-listen:$port", "--no-xterm", "--log", $bothLog `
        -RedirectStandardInput $inFile -RedirectStandardOutput $outFile -PassThru -NoNewWindow
    try {
        Start-Sleep -Milliseconds 600
        $client = [System.Net.Sockets.TcpClient]::new()
        $client.Connect('127.0.0.1', $port)
        $s = $client.GetStream()
        Read-Text $s 256 3000 | Out-Null
        $b = [System.Text.Encoding]::ASCII.GetBytes($recv)
        $s.Write($b, 0, $b.Length); $s.Flush()
        Start-Sleep -Milliseconds 500
        $client.Close()
    } finally {
        Stop-Proc $p
    }
    Start-Sleep -Milliseconds 200
    $log = [System.IO.File]::ReadAllText($bothLog)
    Assert-Equal $true $log.Contains("> $sent") '--log: sent block marked with >'
    Assert-Equal $true $log.Contains("< $recv") '--log: received block marked with <'
    Remove-Item $inFile, $outFile, $bothLog -ErrorAction SilentlyContinue
}
#endregion

#region Runner
$tests = @(
    'Test-TcpListenRoundTrip'
    'Test-TcpListenIPv6'
    'Test-Bridge'
    'Test-TcpClientReconnect'
    'Test-ReadOnly'
    'Test-Timestamps'
    'Test-Hex'
    'Test-LogRecv'
    'Test-LogSend'
    'Test-LogConflict'
    'Test-Log'
)

Write-Host "Testing $Convey"
foreach ($t in $tests) { & $t }

if ($script:failures -gt 0) {
    Write-Host "$($script:failures) test(s) failed."
    exit 1
}
Write-Host "All tests passed."
exit 0
#endregion
