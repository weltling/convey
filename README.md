# The convey tool

Hypervisors like Hyper-V provide a functionality to emulate a serial port in the VM, while exposing it as a named pipe to the host Windows machine. Using convey, it is possible to connect to a virtual machine's virtual serial port from the host system using the exposed named pipe. This project specifically aims Linux VMs running on Windows.

# Building

## Visual C++

- Get onto the VC++ shell
- nmake /nologo

## Clang

- Get onto the VC++ shell
- nmake /nologo CXX="c:\Program Files\LLVM\bin\clang-cl.exe" LD="c:\Program Files\LLVM\bin\lld-link.exe"


# Preparing for Hyper-V

## On host, conifgure a com port

Assign a named pipe that will be passed as a COM1 into a VM.

`Set-VMComPort -VMName <vm name> -Number 1 -Path \\.\pipe\<pipe name>`

View configured COM ports on a VM.

`Get-VMComPort -VMName <vm name>`

## Inside the VM

Add `console=ttyS0,115200 console=tty0` to the kernel parameters.

## Optional

Configure autologin for `ttyS0` or another terminal device you've chosen.


# Connecting to a VM

## Method 1

- Start the VM.
- Start an elevated cmd window and invoke `convey.exe \\.\pipe\<pipe name>`.

## Method 2

- Before starting the VM, invoke convey with the `--poll` argument.
- Start the VM.

# Debugging Linux kernel

## Prerequisities

- Download the unstripped vmlinux to your local machine, or
- Download the vmlinux and the debug symbols.
- Download the kernel sources corresponding to the given kernel build.

## On host, create a PTY mapping to the VM named pipe

Invoke WSL on an elevated console and run

`socat PTY,link=/tmp/my-vm-pty,raw,echo=0 SYSTEM:"while true; do ./convey.exe //./pipe/<pipe name>; sleep 0.1; done"`

## Inside the VM, bring the kernel into debug mode

- `echo ttyS0 > /sys/module/kgdboc/parameters/kgdboc`
- `echo g > /proc/sysrq-trigger`

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

Alternatively, unpack kernel sources under /usr/src/kernel or where ever else the kernel was built. 

## GDB tells `Remote replied unexpectedly to 'vMustReplyEmpty': vMustReplyEmpty`

Forgot to bring kernel into the debugging mode?

## Not all frames are resolved

Add `nokaslr` to the kernel parameters.


# Links

- https://www.kernel.org/doc/html/latest/admin-guide/serial-console.html
- https://www.kernel.org/doc/html/v4.17/dev-tools/kgdb.html
- https://www.elinux.org/Debugging_The_Linux_Kernel_Using_Gdb
- http://man7.org/linux/man-pages/man1/stty.1.html
- https://linux.die.net/man/1/socat
- https://stackoverflow.com/questions/14584504/problems-to-connect-gdb-over-an-serial-port-to-an-kgdb-build-kernel

# TODO

- Check VMWare and VirtualBox.
- Check other things like Windows VM or any other possible counter part exposing named pipes.
- <strike>Add console options for more flexibility.</strike>
- Implement sending/receiving a file.
- ...

