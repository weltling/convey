#Requires -Version 5
# Integration tests for convey's TCP transport and bridge mode.
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

Write-Host "Testing $Convey"
Test-TcpListenRoundTrip
Test-Bridge
Test-TcpClientReconnect

if ($script:failures -gt 0) {
    Write-Host "$($script:failures) test(s) failed."
    exit 1
}
Write-Host "All tests passed."
exit 0
