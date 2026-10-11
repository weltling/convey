# The convey tool

[Convey](https://github.com/weltling/convey) is an interprocess communication tool with capabilities to communicate through a named pipe, a serial port or a TCP connection. Notable features include the communication with Hyper-V virtual machines through an emulated COM port. Simplicity of use is the main focus of this tool.

Convey is distributed under the BSD 2-clause license.


# Building

## Visual C++

- Get onto the VC++ shell
- nmake /nologo

## Clang

- Get onto the VC++ shell
- nmake /nologo CXX="c:\Program Files\LLVM\bin\clang-cl.exe" LD="c:\Program Files\LLVM\bin\lld-link.exe"


# Usage with a physical COM port

Using a physical COM port is as simple as invoking the tool with the COM port name.

- Invoke `convey.exe COM<num>`
- For the port with number >= 10, use `\.\COM<num>`

There's no difference whether it's a native COM port or a USB to COM convertor. As long as the COM port appears under the device manager, it is usable.

Run `convey.exe --list-serial-ports` to see the serial ports currently present on the system.


# Usage with Hyper-V

Hypervisors like Hyper-V provide a functionality to emulate a serial port in the VM, while exposing it as a named pipe to the host Windows machine. Using convey, it is possible to connect to a virtual machine's virtual serial port from the host system using the exposed named pipe.

## Preparing Hyper-V

### On host, configure a COM port

Assign a named pipe that will be passed as a COM1 into a VM.

`Set-VMComPort -VMName <vm name> -Number 1 -Path \\.\pipe\<pipe name>`

View configured COM ports on a VM.

`Get-VMComPort -VMName <vm name>`

### Inside the VM

Add `console=ttyS0,115200 console=tty0` to the kernel parameters. Note that `ttyS0` is what is usually
available on a typical setup. Depending on the hardware and system configuration, this device name can
be different.

### Optional

Configure autologin for `ttyS0` or another terminal device you've chosen.


## Connecting to a VM

### Method 1

- Start the VM.
- Start an elevated cmd window and invoke `convey.exe \\.\pipe\<pipe name>`.

### Method 2

- Before starting the VM, invoke convey with the `--poll` argument.
- Start the VM.


# Usage over TCP

Besides named pipes and COM ports, convey can also communicate over a TCP connection.

## Client and server

- Invoke `convey.exe tcp:<host>:<port>` to connect to a TCP server.
- Invoke `convey.exe tcp-listen:<port>` to accept a single incoming connection.

The `--poll` and `--reconnect` options work here too. `--poll` keeps retrying the connection on startup, `--reconnect` reestablishes it after a drop.

TCP connections are opened with `TCP_NODELAY` for low latency. Keepalive probes start after 30 s of idle and then run every 5 s, so a silently dropped peer is detected in about a minute instead of hanging. Both IPv4 and IPv6 are supported.

## Windows kernel debugging with WinDbg

This targets a Windows guest whose serial port is available on TCP, as done by QEMU, cloud-hypervisor and others. The bridge mode lets WinDbg reach such a serial over TCP target through a named pipe, without any third party virtual COM driver. Convey creates the pipe server and pumps raw bytes between it and the TCP endpoint.

- On host, start the bridge with `convey.exe --bridge --pipe-server \\.\pipe\kd0 tcp:<host>:<port>`.
- Attach WinDbg with `windbg -k com:pipe,port=\\.\pipe\kd0,resets=0,reconnect`.

The bridge carries raw bytes only, so there's no console, no CRLF trimming and no xterm handling. It reconnects on its own, which lets it survive target resets.


# Usage over a Unix domain socket

Convey also speaks AF_UNIX stream sockets, which are available on Windows 10 build 17063 and newer.

- Invoke `convey.exe unix:<path>` to connect to a listening socket.
- Invoke `convey.exe unix-listen:<path>` to create the socket and accept a single connection.

The `<path>` is a normal filesystem path. The listener removes a stale socket file before it binds and removes its own socket file on exit. Only stream sockets on a filesystem path are supported. Windows has no abstract sockets. The `--poll` and `--reconnect` options apply here as they do for TCP.


# Relay between two endpoints

`--relay` pumps raw bytes in both directions between any two endpoints, with no console and no serial control. It generalizes the bridge, which is the special case of a pipe server relayed to one other endpoint. Use it to expose one transport over another, for example a serial port over TCP or a TCP service over a named pipe.

- `convey.exe --relay <A> <B>` opens endpoint A, then endpoint B, and relays bytes until either side closes.

Each endpoint is one of these forms:

- `tcp:<host>:<port>` connects out to a TCP server.
- `tcp-listen:<port>` accepts one incoming TCP connection.
- `unix:<path>` connects to a listening AF_UNIX socket.
- `unix-listen:<path>` creates an AF_UNIX socket and accepts one connection.
- `pipe-listen:<name>` creates a named pipe server and waits for a client, for example `pipe-listen:\\.\pipe\kd0`.
- A bare path such as `\\.\pipe\<name>` or `\\.\COM<n>` opens an existing pipe or serial port.

A listening endpoint blocks until its peer arrives, so a client that must reach endpoint A should connect before endpoint B is opened. Serial endpoints open with their current port settings, since the serial options apply to the interactive console rather than to a relay. For example, `convey.exe --relay pipe-listen:\\.\pipe\kd0 tcp:dbg-host:5555` matches the bridge used for kernel debugging.


# Logging

Convey can log the session to a file, for example to keep a boot or panic log that would otherwise scroll away. `--log` captures the full session. The received stream already includes what you type on an echoing console, but a target that does not echo, or a one way stream, needs the sent stream too.

- `--log <file>` logs the full session into one file, each block marked `>` for sent or `<` for received.
- `--log-recv <file>` logs only the received stream, target to host.
- `--log-send <file>` logs only the sent stream, host to target.
- `--log-append` appends to the log files instead of overwriting them.

The log options may be combined to write several files at once, for example a marked session plus a raw received dump, but each must name a different file. `--log-append` applies to all of them. For example, `convey.exe --log session.log \\.\pipe\<pipe name>`. The logs stay open across `--reconnect` so they are not truncated on every reconnect.


# Read only monitor mode

Pass `--read-only` to watch an endpoint without sending anything to it. Convey still shows and logs everything received, but the host to endpoint direction is disabled, so a stray keypress cannot interrupt a boot or another person's session. It applies to the interactive console. `--bridge` is always a two way relay and ignores it.

For example, `convey.exe --read-only --log boot.log tcp:10.0.0.5:4445`.


# Timestamps

Pass `--timestamps` to prefix each received line with a local time stamp in `[HH:MM:SS]` form. It stamps the stream shown on stdout, which helps correlate boot or hang timing on the host side. The log files are left raw.

For example, `convey.exe --timestamps tcp:10.0.0.5:4445`.


# Hex view

Pass `--hex` to show the received stream as a hex dump instead of text, in the familiar `offset  hex bytes  |ascii|` form. Only the display changes, so the log files stay raw. It combines with `--timestamps`, which stamps each dump row.

For example, `convey.exe --hex tcp:10.0.0.5:4445`.


# Line endings

By default convey sends what you type unchanged, except that `--no-xterm` drops the trailing line feed so a serial console sees a bare carriage return on Enter. Three options make the outgoing translation explicit and override that default:

- `--raw` sends input with no newline translation.
- `--lf` translates sent CRLF newlines to LF.
- `--crlf` translates sent LF newlines to CRLF.

Only the host to endpoint direction is affected. Received bytes and the log files stay raw.


# Reconnecting

With `--reconnect` convey reestablishes the connection after it drops and resumes the session. Retries use an exponential backoff that starts at 300 ms and doubles up to 5 s between attempts, and convey prints `reconnecting...` and `reconnected` on stderr so the status stays visible without disturbing the data on stdout. By default it retries indefinitely. `--max-retries N` gives up after N attempts.

The related `--poll` option keeps retrying the initial connection for the given number of seconds before the session begins.

Pass `--wait` to wait for the endpoint to appear at startup, retrying forever instead of for a fixed number of seconds. Convey prints `waiting for '<endpoint>'` and then `connected` once it is reachable. This is handy for a serial device that is not plugged in yet, or a listener that starts later.


# Commands

While a session is running, press `Ctrl+A` followed by a command key:

- `q` quits convey.
- `b` sends a serial BREAK. It is held for the `--break-ms` duration, 300 ms by default, and only works on a physical COM port. On a named pipe or TCP connection convey reports that break needs a serial port.
- `r` sends a reset pulse on the serial DTR line. It deasserts DTR for the `--reset-ms` time, 120 ms by default, then reasserts it, which resets many boards. It works only on a physical COM port.
- `m` shows the modem line status, the CTS, DSR, DCD and RI inputs. It works only on a physical COM port.
- `h` toggles the hex view of the received stream.
- `e` toggles local echo, showing typed input on targets that do not echo.
- `l` toggles logging on and off. With no `--log` configured it starts logging to an automatically named `convey-<timestamp>.log` session file in the working directory.
- `c` clears the screen.
- `x` sends a file. It prompts for a path and streams the file to the target.
- `.` prints the connection status. It shows the transport, endpoint, connected state, hex and read only flags, the received and sent byte counts and the connection uptime.
- `?` lists the available commands.

The same list is available without starting a session by running `convey --help-keys`.

A BREAK on the serial console is how a Linux guest receives `Alt+SysRq`, see the Sysrq section below.

The `--dtr` and `--rts` options set those serial control lines when the port is opened, each taking `on`, `off` or `pulse`. A `pulse` deasserts the line for the `--reset-ms` time, 120 ms by default, and then reasserts it, the same reset pulse `Ctrl+A r` sends for DTR.

The command prefix defaults to `Ctrl+A` and can be changed with `--escape`, which accepts a control key spec such as `^]`, `ctrl-]`, `ctrl+x` or `C-x`. This helps when `Ctrl+A` collides with a shell or readline binding.

By default convey shows your typing only because the target echoes it back. On a target that does not echo, such as a raw line, a bootloader or `--read-only` monitoring, pass `--local-echo` or toggle it with `Ctrl+A e` to echo typed input locally.


# Sending a file

Convey can stream a local file to the target as raw bytes, which is handy for pushing a script or config onto a board whose only link is the serial console. The receiving side needs something that consumes the bytes, typically a shell reading the console, for example `base64 -d > file` followed by the streamed data. It is a console convenience, not a transfer protocol, and it must not be pointed at a `--bridge` or kernel debug channel.

- `--send-file <file>` streams the file once after connecting.
- `Ctrl+A x` prompts for a path and streams it during a session.
- `--send-delay <ms>` waits between sent lines, so a line buffered shell or bootloader does not drop input. Hardware flow control with `--flow-control rts/cts` achieves the same without a fixed delay.

The sent bytes follow the `--raw`/`--lf`/`--crlf` line ending choice.

`--send-init <string>` sends a fixed string to the target after every connect, including reconnects, which is handy for a login or unlock sequence. It accepts `\r`, `\n`, `\t`, `\0`, `\\` and `\xHH` escapes, for example `--send-init "root\r"`.


# Waiting for a pattern

`--exit-on <regex>` watches the received stream and exits with status 0 as soon as the ECMAScript regular expression matches. Together with `--send-init` this makes convey a scriptable step: connect, send a command, and wait for a prompt or a ready marker. `--exit-timeout <seconds>` exits with a nonzero status if the pattern has not appeared in time. The default of 0 waits forever. The match runs on the raw received bytes, before the hex or timestamp view.

For example, `convey.exe --send-init "\r" --exit-on "login:" --exit-timeout 30 tcp:10.0.0.5:4445`.

For an interactive login or unlock dialog, pair `--expect <regex>` with `--reply <string>`. Each time the pattern is seen convey sends the reply and advances to the next pair, then keeps the session running. Pairs are matched in order, the reply accepts the same escapes as `--send-init`, and `--expect-timeout <seconds>` fails if the sequence does not complete in time.

For example, `convey.exe --expect "login:" --reply "root\r" --expect "Password:" --reply "hunter2\r" --expect-timeout 30 tcp:10.0.0.5:4445`.

In an interactive console `Ctrl+C` is passed through to the target. When there is no interactive console to type into, with `--no-xterm`, redirected input or `--bridge`, `Ctrl+C` quits convey after restoring the console and closing the transport.


# Debugging Linux kernel

## Prerequisites

- Download the unstripped vmlinux to your local machine, or
- Download the vmlinux and the debug symbols.
- Download the kernel sources corresponding to the given kernel build.

## On host, create a PTY mapping to the VM named pipe

Invoke WSL on an elevated console and run

`socat PTY,link=/tmp/my-vm-pty,raw,echo=0 SYSTEM:"while true; do ./convey.exe //./pipe/<pipe name>; sleep 0.1; done"`

## VM setup

Add `nokaslr` to the kernel command line.

See also a more detailed documentation on the [kgdb](https://www.kernel.org/doc/html/v4.17/dev-tools/kgdb.html) page.

### Turn on kernel debug mode

#### Method 1

Inside the VM, execute the commands below:

- `echo ttyS0 > /sys/module/kgdboc/parameters/kgdboc`
- `echo g > /proc/sysrq-trigger`

#### Method 2

Add a suitable configuration to the kernel command line, for example `kgdboc=ttyS0,115200 kgdbwait`.

## On host, start debugging

Run another WSL shell and invoke

```
$ gdb ./vmlinux
(gdb) set serial baud 115200
(gdb) target remote /tmp/my-vm-pty
```

Here you are. This doesn't need an elevated console.


# Troubleshooting & Tricks

## Disable input echoing

`stty -F /dev/ttyS0 -echo`

## The serial screen size is too small

Use stty to set the desired columns and rows number, for example

`stty columns 235 rows 62`

## Pointing gdb to the sources

`(gdb) set substitute-path /sources/were/compiled/here /put/sources/here`

To add multiple folders to be searched by GDB, use

`(gdb) set dir /path/to/base/dir`

Alternatively, unpack kernel sources under /usr/src/kernel or wherever else the kernel was built.

## GDB tells `Remote replied unexpectedly to 'vMustReplyEmpty': vMustReplyEmpty`

Forgot to bring kernel into the debugging mode?

## Not all frames are resolved

Add `nokaslr` to the kernel parameters.

# Sysrq cannot be sent due to the lockdown

If the `lockdown=` option is on the kernel cmdline, it has to be removed.

Some distributions also allow disabling lockdown at runtime. In a VM, `Alt+SysRq+x` can be sent by:

```
$ echo 1 > /proc/sys/kernel/sysrq
$ echo x > /proc/sysrq-trigger
```

# Links

- https://www.kernel.org/doc/html/latest/admin-guide/serial-console.html
- https://www.kernel.org/doc/html/v4.17/dev-tools/kgdb.html
- https://www.elinux.org/Debugging_The_Linux_Kernel_Using_Gdb
- http://man7.org/linux/man-pages/man1/stty.1.html
- https://linux.die.net/man/1/socat
- https://stackoverflow.com/questions/14584504/problems-to-connect-gdb-over-an-serial-port-to-an-kgdb-build-kernel
- https://unix.stackexchange.com/questions/125183/how-to-find-which-serial-port-is-in-use
- https://sourceware.org/gdb/onlinedocs/gdb/Source-Path.html

# TODO

- Check VMWare and VirtualBox.
- Check other things like Windows VM or any other possible counter part exposing named pipes.
- <strike>Add console options for more flexibility.</strike>
- Implement sending/receiving a file.
- ...

