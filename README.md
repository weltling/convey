# The convey tool

[Convey](https://github.com/weltling/convey) is an inter-process communication tool with capabilities to communicate through a named pipe, a serial port or a TCP connection. Notable features include the communication with Hyper-V virtual machines through an emulated COM port. Simplicity from the use point is the most point of focus for this tool.

Convey is distributed under the BSD 2-clause license.


# Building

## Visual C++

- Get onto the VC++ shell
- nmake /nologo

## Clang

- Get onto the VC++ shell
- nmake /nologo CXX="c:\Program Files\LLVM\bin\clang-cl.exe" LD="c:\Program Files\LLVM\bin\lld-link.exe"


# Usage with a physical COM port

The physical COM port usage is a simple as invoking the tool with the COM port name.

- Invoke `convey.exe COM<num>`
- For the port with number >= 10, use `\.\COM<num>`

There's no difference whether it's a native COM port or a USB-to-COM convertor. As long as the COM port appears under the device manager, it is usable.


# Usage with Hyper-V

Hypervisors like Hyper-V provide a functionality to emulate a serial port in the VM, while exposing it as a named pipe to the host Windows machine. Using convey, it is possible to connect to a virtual machine's virtual serial port from the host system using the exposed named pipe.

## Preparing Hyper-V

### On host, conifgure a com port

Assign a named pipe that will be passed as a COM1 into a VM.

`Set-VMComPort -VMName <vm name> -Number 1 -Path \\.\pipe\<pipe name>`

View configured COM ports on a VM.

`Get-VMComPort -VMName <vm name>`

### Inside the VM

Add `console=ttyS0,115200 console=tty0` to the kernel parameters. Note, that `ttyS0` is what is usually
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

The `--poll` and `--reconnect` options work here too. `--poll` keeps retrying the connection on startup, `--reconnect` re-establishes it after a drop.

## Windows kernel debugging with WinDbg

This targets a Windows guest whose serial port is available on TCP, as done by QEMU, cloud-hypervisor and others. The bridge mode lets WinDbg reach such a serial-over-TCP target through a named pipe, without any third-party virtual COM driver. Convey creates the pipe server and pumps raw bytes between it and the TCP endpoint.

- On host, start the bridge with `convey.exe --bridge --pipe-server \\.\pipe\kd0 tcp:<host>:<port>`.
- Attach WinDbg with `windbg -k com:pipe,port=\\.\pipe\kd0,resets=0,reconnect`.

The bridge carries raw bytes only, so there's no console, no CRLF trimming and no xterm handling. It reconnects on its own, which lets it survive target resets.


# Logging

Convey can log the session to a file, for example to keep a boot or panic log that would otherwise scroll away. `--log` captures the full session; the received stream already includes what you type on an echoing console, but a non-echoing target (or a one-way stream) needs the sent stream too.

- `--log <file>` logs the full session into one file, each block marked `>` (sent) or `<` (received).
- `--log-recv <file>` logs only the received stream (target to host).
- `--log-send <file>` logs only the sent stream (host to target).
- `--log-append` appends to the log files instead of overwriting them.

The log options may be combined to write several files at once (for example a marked session plus a raw received dump), but each must name a different file. `--log-append` applies to all of them. For example, `convey.exe --log session.log \\.\pipe\<pipe name>`. The logs stay open across `--reconnect` so they are not truncated on every reconnect.


# Read-only monitor mode

Pass `--read-only` to watch an endpoint without sending anything to it. Convey still shows and logs everything received, but the host-to-endpoint direction is disabled, so a stray keypress cannot interrupt a boot or another person's session. It applies to the interactive console; `--bridge` is always a two-way relay and ignores it.

For example, `convey.exe --read-only --log boot.log tcp:10.0.0.5:4445`.


# Timestamps

Pass `--timestamps` to prefix each received line with a local time stamp in `[HH:MM:SS]` form. It stamps the stream shown on stdout, which helps correlate boot or hang timing on the host side. The log files are left raw.

For example, `convey.exe --timestamps tcp:10.0.0.5:4445`.


# Hex view

Pass `--hex` to show the received stream as a hex dump instead of text, in the familiar `offset  hex bytes  |ascii|` form. Only the display changes, so the log files stay raw. It combines with `--timestamps`, which stamps each dump row.

For example, `convey.exe --hex tcp:10.0.0.5:4445`.


# Debugging Linux kernel

## Prerequisities

- Download the unstripped vmlinux to your local machine, or
- Download the vmlinux and the debug symbols.
- Download the kernel sources corresponding to the given kernel build.

## On host, create a PTY mapping to the VM named pipe

Invoke WSL on an elevated console and run

`socat PTY,link=/tmp/my-vm-pty,raw,echo=0 SYSTEM:"while true; do ./convey.exe //./pipe/<pipe name>; sleep 0.1; done"`

## VM setup

Add `nokaslr` to the kernel command line.

See also a more detailed documentation on the (kgdb)[https://www.kernel.org/doc/html/v4.17/dev-tools/kgdb.html] page.

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

Alternatively, unpack kernel sources under /usr/src/kernel or where ever else the kernel was built. 

## GDB tells `Remote replied unexpectedly to 'vMustReplyEmpty': vMustReplyEmpty`

Forgot to bring kernel into the debugging mode?

## Not all frames are resolved

Add `nokaslr` to the kernel parameters.

# Sysrq cannot be sent due to the lockdown

If the `lockdown=` option is on the kernel cmdline, it has to be removed.

Some distribution might also allow to disable lockdown at runtime. In a VM, `Alt+SysRq+x` can be sent by:

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

