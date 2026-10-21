# MSCPHUB User's Guide

Version X01-15. StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan).

MSCPHUB is a program for Linux. It gives disks to OpenVMS computers on the
same network. The OpenVMS computer sees a new disk, for example
`$42$DKA100:`, and uses it like its own disk: `INITIALIZE`, `MOUNT`, `COPY`,
`BACKUP`. The data stays in a file or on a disk of the Linux computer.

Nothing is installed on OpenVMS. MSCPHUB speaks the cluster protocol of
OpenVMS (NISCA, the protocol of PEDRIVER). It is not a cluster member and it
has no votes.

This guide shows the whole way, step by step. The reference of every
parameter and every message is in the help library (`mscphub /HELP`) and in
the manual page (`man 8 mscphub`).

## 1. What you need

- A Linux computer with a kernel 5.10 or later (Debian 11 and later).
- An Ethernet connection to the LAN of the OpenVMS computers: the same
  switch, VLAN or bridge.
- The OpenVMS computers form a VMScluster over the LAN (PEDRIVER runs on
  them). One OpenVMS computer alone is a cluster too, if it was set up with
  `CLUSTER_CONFIG` and runs PEDRIVER.
- root on Linux.

## 2. Install

MSCPHUB is built from its kit. It needs the StarLet package and liburing:

```
# apt install cmake gcc pkg-config liburing-dev
# tar xzf MSCPHUB-X01-09.tar.gz
# cd MSCPHUB-X01-09
# cmake -S . -B build
# cmake --build build -j
# ctest --test-dir build
# cmake --install build
```

Install only when `ctest` says "100% tests passed". The HELP package is
optional: with it, `/HELP` shows the full help library.

What is installed (with the default prefix `/opt/mscphub`):

| File | What it is |
|---|---|
| `/opt/mscphub/sbin/mscphub` | the server |
| `/etc/opt/mscphub/mscphub.conf.example` | a sample configuration |
| `/lib/systemd/system/mscphub.service` | the systemd unit, not enabled |
| `/opt/mscphub/share/man/man8/mscphub.8` | the manual page |
| `/opt/mscphub/share/help/mscphub.hlb` | the help library (with HELP) |
| `/opt/mscphub/share/doc/mscphub/` | this guide, README |

The server and its page are not on the search paths of the shell. Add
them, for example in `/root/.profile`:

```
export PATH=$PATH:/opt/mscphub/sbin
export MANPATH=$MANPATH:/opt/mscphub/share/man
```

Another place: `cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local`
(then the configuration is in `/usr/local/etc`).

To remove it: `cmake --build build --target uninstall`.

## 3. Find the facts of the cluster

On one OpenVMS computer of the cluster, write down four things.

**The group number:**

```
$ MCR SYSMAN CONFIGURATION SHOW CLUSTER_AUTHORIZATION
```

**The password hash.** It is in `SYS$SYSTEM:CLUSTER_AUTHORIZE.DAT`:

```
$ DUMP SYS$SYSTEM:CLUSTER_AUTHORIZE.DAT
```

Take the quadword at offset 8 and write its octets from the lowest one. The
longwords `00000000 8EF54F24` there give `/AUTHORIZE=5A3C9E1700000000`.

**The names and the system IDs in use:**

```
$ SHOW CLUSTER
$ MCR SYSGEN SHOW SCSSYSTEMID
```

Choose for MSCPHUB a name (`/NODE`, up to 6 letters) and a system ID
(`/SYSTEMID`, 1 to 65535) that no computer of the cluster has. The server
checks this itself when it starts and stops if it finds a clash.

**An allocation class** for the disks, 1 to 255, that the cluster does not
use for disks of the same names. With `/ALLOCLASS=42` the disk `DKA100` is
`$42$DKA100:` on every OpenVMS computer.

## 4. Prepare the disks

A disk is a file or a block device of Linux.

A new empty file of 2 GB:

```
# mkdir -p /srv/mscphub
# truncate -s 2G /srv/mscphub/dka100.img
```

Such a file is sparse: it takes no space yet, and the first write to
every part of it is slower. For a disk that must be fast, write the
file full once instead:

```
# dd if=/dev/zero of=/srv/mscphub/dka100.img bs=1M count=2048
```

Put the files on ext4 or XFS: the server reads and writes them with
direct I/O, as a block device. On a file system without it (tmpfs of an
old kernel, some network file systems) it uses the page cache and says
`NODIRECT`; it works, but every write costs more. A partition or an LVM
volume is still a little faster than a file.

A block device - a whole disk, a partition, an LVM volume, a loop device,
an iSCSI LUN - must not be mounted or used by Linux. The server takes it
for itself and refuses one that is in use (`DEVBUSY`).

A disk of 4096-octet sectors works too. A partition on it must start on
a physical sector, or every write is slow (the server says `MISALIGN`);
`parted` and `fdisk` start partitions on 1 MB by default.

The CD of OpenVMS (an ISO file) is served read only, as an RRD42:

```
/UNIT2=DKA300,/srv/mscphub/AXPVMS083.iso,RO
```

## 5. Write the configuration

```
# cp /etc/opt/mscphub/mscphub.conf.example /etc/opt/mscphub/mscphub.conf
# vi /etc/opt/mscphub/mscphub.conf
```

A complete example:

```
! MSCPHUB of our cluster
/INTERFACE=eth1
/NODE=MHUB
/SYSTEMID=65200
/GROUP=4242
/AUTHORIZE=5A3C9E1700000000
/ALLOCLASS=42
/UNIT0=DKA100,/srv/mscphub/dka100.img
/UNIT1=DKA200,/dev/vg0/vmsdata
/UNIT2=DKA300,/srv/mscphub/AXPVMS083.iso,RO
```

Check it first without sending anything: `/PASSIVE` only listens.

```
# mscphub /CONFIG=/etc/opt/mscphub/mscphub.conf /PASSIVE
```

After the 4 seconds of `/LISTEN` you see a `NEWNODE` for every OpenVMS
computer. Press Ctrl-C. No `NEWNODE`? See section 9.

## 6. Start the service

```
# systemctl enable --now mscphub
# journalctl -u mscphub -f
```

In the journal, in order:

```
%MSCPHUB-I-UNITCFG, unit $42$DKA100 is /srv/mscphub/dka100.img, 4194304 blocks, a file, sector 512, RZ29, SLUN 4000
%MSCPHUB-I-STARTUP, MSCPHUB X01-09, node MHUB, system ID 65200, group 4242, on eth1 (...)
%MSCPHUB-I-ANNOUNCE, node MHUB announced on eth1, 2 other nodes seen in group 4242
%MSCPHUB-I-CHOPEN, channel to node ALPHA1 (65534) is open, ...
%MSCPHUB-I-VCOPEN, virtual circuit to node ALPHA1 (65534) is open: VMS V8.3 on ALPH, ...
%MSCPHUB-I-CONOPEN, connection VMS$DISK_CL_DRVR of ALPHA1 (65534) to MSCP$DISK is open, credit 10/16
```

The unit is not started at boot until you enable it, and it does not start
without `/etc/opt/mscphub/mscphub.conf`. This is on purpose: a server that
starts by itself talks to the cluster at once.

## 7. Use the disks on OpenVMS

A few seconds after `CONOPEN`:

```
$ SHOW DEVICE $42$DKA
$ INITIALIZE $42$DKA100: DATA1
$ MOUNT/SYSTEM $42$DKA100: DATA1
$ COPY *.* $42$DKA100:[000000]
```

On a Linux disk of 4096-octet sectors, give the cluster yourself, 8 or a
multiple of 8: `INITIALIZE/CLUSTER=16 $42$DKA100: DATA1`. Then the files
fill whole sectors and are written without reading first. OpenVMS takes
16 by itself on a disk of more than 50000 blocks, 1 on a smaller one.

Two or more computers of the cluster can use one disk together:
`MOUNT/CLUSTER $42$DKA100: DATA1`.

Before you stop the server, dismount its disks on OpenVMS. A disk still
mounted when the server stops waits for it (the computer retries); when
the server comes back, the work goes on.

## 8. Look at the server

```
# systemctl kill -s USR1 mscphub
# journalctl -u mscphub -n 30
```

The server writes `UPTIME`, one `NODESTATS` for each OpenVMS computer, then
the counters: frames, transport, data, and for each disk the reads, the
writes and the commands running now.

To stop: `systemctl stop mscphub`. The server says goodbye (BYE) to the
cluster, and the OpenVMS computers close their circuits to it at once.

To pin the server to a CPU or to change the start line, use a drop-in, not
the unit file:

```
# systemctl edit mscphub
[Service]
CPUAffinity=3
```

## 9. When something is wrong

Read the last messages of the journal. Every message has a severity letter:
`I` information, `W` warning, `E` error, `F` fatal - the server stops.
`mscphub "/HELP=Messages NAME"` says what a message means and what to do.

| You see | Check |
|---|---|
| `F-NOPARAM`, `F-IVPARAM` | the parameter it names, in the configuration file |
| `F-SYSERR, socket(AF_PACKET), errno=1` | run as root |
| `F-SYSERR, ioctl(SIOCGIFINDEX), errno=19` | the name of `/INTERFACE` |
| `F-IDCLASH`, `F-NAMECLASH` | choose another `/SYSTEMID` or `/NODE` |
| `F-DEVBUSY` | the device is mounted or used on Linux |
| `W-MISALIGN` | the partition does not start on a physical sector: make a new one on 1 MB |
| `I-NODIRECT` | the file system of the file has no direct I/O: move the file to ext4 or XFS |
| `%UTILS-E: ... Skip unrecognized option` | a misspelled parameter: it is NOT used, the server runs without it |
| no `NEWNODE` | the cable, the VLAN or bridge, `/GROUP`; `tcpdump -i eth1 ether proto 0x6007` |
| `W-AUTHFAIL` | `/AUTHORIZE` is not the hash of the cluster password |
| `CHOPEN` and `CHCLOSE` again and again | frames are lost: the switch, the VLAN |
| no `CONOPEN` | `/UNITn` lines; on OpenVMS, the process CONFIGURE (`SHOW SYSTEM`) |
| `E-IOERR` | the Linux file system or disk: full, read-only, failed |
| `W-IOSTUCK` | the Linux disk hangs: `dmesg` |
| slow writes, fast reads | frames lost from OpenVMS to Linux: see `TRSTATS` |

`/TRACE` writes every frame to the log. Use it for a short time only, and
keep the log if you report a problem.

## 10. Safe use

- Never give the same file or device to two servers, and never mount it on
  Linux while it is served.
- A WRITE is answered when its data is on the Linux disk (`/SYNC=DSYNC`, the
  default; `/SYNC=GROUP` is as safe and faster under many writes).
  `/UNSAFE` and `/SYNC=NONE` are for tests only.
- Back up the image files as you back up any disk: with the server
  stopped, or with the disk dismounted on OpenVMS.
