# MSCPHUB -- MSCP disk server for OpenVMS nodes

MSCPHUB runs on Linux. It lets OpenVMS nodes on the same LAN use
Linux files and disks as their own disks. To an OpenVMS node it looks
like a disk controller (like an HSC): the node sees the disks with
SHOW DEVICE and mounts them with MOUNT.

MSCPHUB talks NISCA, the protocol of PEDRIVER, over the Ethernet.
It is not a member of the VMScluster and it has no votes. The
OpenVMS nodes need no changes.

Format:

```
mscphub /CONFIG=file
mscphub /INTERFACE=if /NODE=name /SYSTEMID=n /GROUP=n /AUTHORIZE=hex [/qualifiers]
```

A parameter can be given on the command line or in the file of
/CONFIG. You can write --NAME=value instead of /NAME=value.

This is version X01-09, the first release (v1). The server announces
itself on the LAN and checks that no node already uses its name or
its system ID. It opens a channel and a virtual circuit to every
OpenVMS node of the group; SHOW CLUSTER on the node shows the server.
The disks of /UNIT0 ... /UNIT7 appear on the node in SHOW DEVICE.
The node can INITIALIZE and MOUNT them, read and write them: COPY,
BACKUP, ANALYZE/DISK_STRUCTURE work as on a local disk. Several nodes
can mount one disk with MOUNT/CLUSTER.

A WRITE is answered only when its data is on the disk of the Linux
host (fdatasync). OpenVMS never asks a disk to flush its cache, so
this is the only safe way. /UNSAFE turns it off for tests.

## /CONFIG -- read the parameters from a file

```
/CONFIG=file
```

The file has one parameter on a line, like on the command line. A
line that starts with ! is a comment:

```
! MSCPHUB of our cluster
/INTERFACE=mh0
/NODE=MHUB
/SYSTEMID=65200
/GROUP=4242
/AUTHORIZE=5A3C9E1700000000
```

## /INTERFACE -- the LAN interface

```
/INTERFACE=name
```

The Linux interface the OpenVMS nodes are on, for example eth1 or a
tap device in a bridge. Mandatory. The server needs CAP_NET_RAW
(run it as root). The MAC address of the interface is not changed.

## /NODE -- the SCSNODE of the server

```
/NODE=name
```

The node name the OpenVMS nodes see, 1 to 6 characters: A-Z, 0-9,
$ and _. Mandatory. It must not be the name of any other node in the
group.

## /SYSTEMID -- the SCSSYSTEMID of the server

```
/SYSTEMID=number
```

The SCSSYSTEMID, 1 to 65535. Mandatory. Its value must differ from
the SCSSYSTEMID of every other node of the group. NISCA tells the
nodes apart by the low 16 bits of the SCSSYSTEMID only.

## /GROUP -- the cluster group number

```
/GROUP=number
```

The cluster group number of the OpenVMS nodes, 1 to 65279. Mandatory.
On OpenVMS it is in SYS$SYSTEM:CLUSTER_AUTHORIZE.DAT.

## /AUTHORIZE -- the cluster password

```
/AUTHORIZE=16 hexadecimal digits
```

The cluster password, as the 8-byte hash OpenVMS keeps in
CLUSTER_AUTHORIZE.DAT. Mandatory. To find it, dump the file on an
OpenVMS node:

```
$ DUMP SYS$SYSTEM:CLUSTER_AUTHORIZE.DAT
```

The hash is the quadword at offset 8, written byte by byte. The
longword 8EF54F24 there is written 5A3C9E1700000000.

## /LISTEN -- seconds of silence at start-up

```
/LISTEN=seconds     (default 4)
```

Before it sends anything, the server listens for this many seconds.
It learns the other nodes from their HELLO messages. If a node uses
the name or the system ID of the server, the server stops.

## /ALLOCLASS -- the allocation class of the disks

```
/ALLOCLASS=number     (default 0)
```

The allocation class of the server, 0 to 255. With /ALLOCLASS=42 the
OpenVMS nodes name the disk DKA100 of the server $42$DKA100. With 0
they name it MHUB$DKA100 (the SCSNODE of the server). Do not change
it while OpenVMS nodes use the disks. Do not give a class that
OpenVMS nodes of the cluster use for disks with the same names.

## /UNIT0 ... /UNIT7 -- the disks

```
/UNIT0=name,path[,RO][,MEDIA=medium][,SECTOR=octets]
```

A disk the server serves. The name is DKcn: DK, a controller letter
A to Z and a number 0 to 9999, for example DKA100. The path is:

- a Linux file (an image, sparse or not). Its size at start-up, in
  blocks of 512 octets, is the size of the disk. The server takes the
  file for itself (flock): a second server cannot serve it too. It
  reads and writes the file with direct I/O (O_DIRECT) when the file
  system allows it, else through the page cache (NODIRECT).
- a block device: a disk, a partition, an LVM volume, a loop device,
  an iSCSI LUN. The server opens it with O_DIRECT and O_EXCL: a device
  that is mounted on the Linux host, or that another server has, is
  refused (DEVBUSY). A device of 4096-octet sectors works: a write
  that does not fill whole sectors is done as read-modify-write.
  A partition that does not start on a physical sector is warned of
  (MISALIGN).

  OpenVMS writes in blocks of 512 octets and knows nothing of 4096-octet
  sectors. To have most of its writes fill whole sectors, INITIALIZE
  the disk with a cluster of 8 or a multiple of 8:
  INITIALIZE/CLUSTER=16 $42$DKA100: DATA1. OpenVMS does so by itself on
  a disk of more than 50000 blocks; on a smaller one it takes 1. The
  writes of one block (file headers, the bitmap, directories) stay
  read-modify-write - as they would in a 4096-octet disk of 512
  emulation.

Options, in any order:

- RO -- read only. The node mounts it write locked.
- MEDIA=medium -- what the node is told the disk is: two or three
  letters and a number, for example RZ29 or RRD42. The default is
  RZ29, and RRD42 (a CD-ROM) with RO.
- SECTOR=octets -- take a file as a disk of 512, 1024, 2048 or 4096
  octet sectors. Only for tests of 4K disks; a block device tells its
  sector itself.

Examples:

```
/ALLOCLASS=42
/UNIT0=DKA100,/srv/vms/data1.img
/UNIT1=DKA200,/dev/vg0/vmsdata
/UNIT2=DKA300,/srv/iso/axpvms083.iso,RO
```

The CD image of OpenVMS (an ISO file of 2048-octet sectors) is served
as is: the node mounts it with MOUNT, it is an RRD42. Do not change a
file or a device while the server serves it.

The server does its disk I/O with io_uring. It needs a Linux kernel
of the last five years (5.10 or later); with io_uring turned off
(sysctl kernel.io_uring_disabled) it stops at start-up with SYSERR.

## /SYNC -- when a WRITE is on disk

```
/SYNC=DSYNC | GROUP | NONE     (default DSYNC)
```

OpenVMS never asks a disk to flush its cache, so the server answers a
WRITE only when its data is on the disk of the Linux host.

- DSYNC -- every write goes to the disk by itself (RWF_DSYNC).
- GROUP -- the writes that end while the disk is flushing wait for
  the next flush and are answered together: fewer flushes when many
  writes come at once. As safe as DSYNC.
- NONE -- the same as /UNSAFE.

## /CREDITS -- commands a node may send at once

```
/CREDITS=number     (default 16)
```

How many commands each OpenVMS node may have in progress on the disk
server at a time, 2 to 64. An OpenVMS disk server gives 32
(MSCP_CREDITS). More lets a busy node keep more reads and writes
going; it is given when the node connects.

## /ACK -- when the server acknowledges alone

```
/ACK=HOLD | FLUSH     (default HOLD)
```

Every packet a node sends must be acknowledged. Most of the time the
acknowledgement goes in a packet of the server - the answer to the
command, the data of a READ. When the server has nothing to send, it
sends the acknowledgement alone, in a short packet of its own.

- FLUSH -- the short packet goes as soon as the server has taken all
  the frames that came in.
- HOLD -- the short packet goes only when the node asks for it at
  once, after 4 packets, or after 20 ms at most. Otherwise the
  acknowledgement waits for the answer of the server. Fewer packets
  for the node to take in.

HOLD sends far fewer packets and is a little faster. FLUSH is the
old way; the counters of SIGUSR1 (ACKSTATS, LATSTATS) compare the two.

## /TIMEZONE -- the zone of the time told to the nodes

```
/TIMEZONE=LOCAL | UTC | [+|-]hh[:mm]     (default LOCAL)
```

An OpenVMS node keeps local time. The server tells the nodes its
time when it starts a circuit; SHOW CLUSTER and SCACP show it as the
time the server came up. LOCAL is the zone of the Linux host (TZ,
/etc/localtime), summer time and all. Give an offset, such as
/TIMEZONE=+03:00, when the nodes keep another zone. Only the eye of
the operator sees it; the protocol works with any zone.

## /UNSAFE -- answer a WRITE before its data is on disk

The server answers a write as soon as Linux has taken the data, before
it is on the disk. It is faster, but a crash or a power failure of the Linux host
can lose data the OpenVMS node believes is written. Use it only for
tests. The server says UNSAFE at start-up.

## /PASSIVE -- never transmit

The server only listens and reports the nodes it sees. It opens no
channels and sends nothing at all. This is safe on any LAN, for
example to look at a production cluster.

## /TRACE -- log every frame

Each frame the server receives is written to the log, and so is
every step of every channel.

## /LOGFILE -- the log file

```
/LOGFILE=file
```

Messages go to this file. Without /LOGFILE they go to the standard
error.

## /HELP -- display this description

```
/HELP
/HELP="topic subtopic"
```

Examples:

```
$ mscphub /HELP=/SYSTEMID
$ mscphub "/HELP=Messages IDCLASH"
```

## Signals

```
SIGINT, SIGTERM    stop the server
SIGUSR1            write what the server is doing to the log: UPTIME, a
                   NODESTATS for each node, then the counters (STATS, TRSTATS,
                   ACKSTATS, BLKSTATS, MSCPSTATS, UNITSTATS, BKSTATS,
                   LATSTATS)
```

Under systemd:

```
# systemctl kill -s USR1 mscphub
# journalctl -u mscphub -n 30
```

## Service -- run the server under systemd

The installation puts the unit mscphub.service in place, but does not
enable it: a server that starts by itself begins to talk to the
cluster on its LAN. The unit reads /etc/opt/mscphub/mscphub.conf and
does not start without it.

```
# cp /etc/opt/mscphub/mscphub.conf.example /etc/opt/mscphub/mscphub.conf
# vi /etc/opt/mscphub/mscphub.conf
# systemctl enable --now mscphub
# journalctl -u mscphub -f
```

To pin the server to a CPU, or to give it other parameters, use a
drop-in (systemctl edit mscphub) - do not edit the unit itself.

## Troubleshooting -- what to check first

The server stops at once. Read the last message: it is F (fatal) and
says why. See the topic Messages.

No node is seen: ANNOUNCE says 0 other nodes. Check /GROUP. Check
that the interface is up and in the right bridge or VLAN. On OpenVMS,
check that PEDRIVER runs: MCR SCACP SHOW CHANNEL. Look at the wire:

```
# tcpdump -i mh0 ether proto 0x6007
```

The nodes are seen, but no CHOPEN comes. Look for AUTHFAIL: the
password hash of /AUTHORIZE is not the one of the group. Check that
/PASSIVE is not given. On OpenVMS, MCR SCACP SHOW CHANNEL shows the
channel to the server and its state.

A channel opens and closes again and again (CHOPEN, then CHCLOSE).
Read the reason in CHCLOSE. "nothing heard from the node" means
frames are lost between the node and the server: check the bridge
or the VLAN.

The channel is open, but no VCOPEN comes. The OpenVMS node sends
START over an open channel every 5 seconds. Run the server with
/TRACE and look for "PPD START": if it is there, look for a warning
after it.

The circuit is open, but the disks do not appear on the node. Look
for CONOPEN: the node connects to MSCP$DISK some seconds after the
circuit opens. No CONOPEN: check that /UNITn are given (UNITCFG at
start-up). On OpenVMS, check that the process CONFIGURE runs (SHOW
SYSTEM); it loads the class driver DUDRIVER.

The virtual circuit opens and closes again and again. Read the
reason in VCCLOSE. See the counters (SIGUSR1, message TRSTATS):
many retransmissions mean frames are lost.

MOUNT or COPY fails with an I/O error on the node. Look for IOERR:
the Linux file could not be read or written (a full file system, for
one). Look for MSCPERR and BLKERR too.

Writes are very slow, reads are not. Frames are lost on the way from
the node to the server: the node sends a lost frame again only after
a second or more. Check the bridge, the VLAN, the cables. See
TRSTATS: many "out of order" packets mean lost frames from the node.

## Messages -- the conditions the server signals

Every message has the OpenVMS form:

```
%MSCPHUB-F-IDCLASH, node ALPHA1 (08-00-2B-E5-40-01) uses the system ID low word 65534 of this server
```

The letter after MSCPHUB is the severity: S success, I information,
W warning, E error, F fatal (the server stops).

### STARTUP

The server has started. It shows its version, its name, its system
ID, its group and its interface. No action.

### LISTEN

The server listens before it announces itself. No action.

### ANNOUNCE

The listen time is over. The server now sends HELLO messages. It
shows how many other nodes it has seen. With /PASSIVE it says
"listens only" and sends nothing. No action.

### NEWNODE

The server sees a node for the first time: its name, its system ID,
its adapter and its NISCA version. No action.

### IDCLASH

Another node uses the same low 16 bits of the SCSSYSTEMID as the
server. Two such nodes cannot be told apart on the LAN. The server
stops and sends nothing.

Action: give the server another /SYSTEMID. On OpenVMS, SHOW CLUSTER
or MCR SYSGEN SHOW SCSSYSTEMID shows the IDs in use.

### NAMECLASH

Another node has the same SCSNODE as the server. The server stops
and sends nothing.

Action: give the server another /NODE.

### NOPARAM

A mandatory parameter is missing. Action: add it.

### IVPARAM

A parameter has a wrong value. The message says what is expected.
Action: correct the value.

### SYSERR

A Linux system call failed. The message shows the call and errno.
Most often:

```
socket(AF_PACKET), errno=1      run the server as root (CAP_NET_RAW)
ioctl(SIOCGIFINDEX), errno=19   no such interface
```

### MANYNODES

There are more nodes on the LAN than the server remembers (64). A
node beyond that is not checked for a clash. Action: report it.

### CHOPEN

The channel to a node is open: the handshake is over. The message
shows the CC version of the node, its pipe quota, its buffer size
and its services (offered/required). No action.

### CHCLOSE

The channel to a node is closed. The message says why:

```
BYE received, the node is going down    the node shuts down or crashes
restarted by the node                   the node started a new channel
no answer to the handshake in 5 seconds
nothing heard from the node for 8 seconds
the node has changed its name or system ID
the server is stopping
```

A node that is still up opens the channel again in a few seconds.
Action: none if it opens again. If it does not, see Troubleshooting.

### AUTHFAIL

A node sent a CC message with a wrong cluster password. The message
is ignored, no channel is opened to this node. It is reported once
for each node.

Action: give /AUTHORIZE the hash of the group of this node (see
/AUTHORIZE). Check /GROUP too.

### VCOPEN

The virtual circuit to a node is open: the node has answered our
STACK. The message shows what the node told about itself: software,
version, hardware, the largest datagram and message. SHOW CLUSTER on
this node now shows the server. No action.

### VCCLOSE

The virtual circuit to a node is closed. The message says why:

```
the channel is closed ...                    see CHCLOSE
restarted by the node (START on the open path)   the node rebooted
HOSTSHUT received, the node is shutting down
no acknowledgement after 30 retransmissions  frames are lost
a block transfer the server does not take
the server is stopping
```

After a failure the server restarts the channel at once; the node
opens the circuit again in a few seconds. Action: none if it opens
again. If it closes again and again, see Troubleshooting.

### TRSTATS

The counters of the transport, written after STATS: packets sent,
retransmitted, received, duplicates, out of order, packets of an old
channel, and the widest the send window has been. A few
retransmissions are normal. No action.

### ACKSTATS

The acknowledgements, written after TRSTATS: the /ACK setting, how
many packets of the nodes asked to be acknowledged at once, how many
acknowledgements went in a packet of the server, and how many went
alone - because the node asked, for a packet that came twice, for a
batch of packets (/ACK=HOLD), when all frames were taken
(/ACK=FLUSH), on the 20 ms tick. Fewer alone is better. No action.

### BLKSTATS

The counters of the data, written after TRSTATS: chunks of data sent
to the nodes (READ) and their octets, RET_CNF the nodes sent back
for them, REQ_DAT the server sent (WRITE), chunks of data taken in
and their octets, chunks dropped. No action.

### MSCPSTATS

The state of the disk server, written after BLKSTATS: how many
commands of the nodes are in progress now, and how many octets of
data buffers they hold. When no node uses the disks, both are 0. A
number that stays above 0 with no node connected is a fault of the
server: report it with the log. No action otherwise.

### UNITSTATS

One for each disk, written after MSCPSTATS: to how many nodes the
disk is ONLINE (mounted, or being mounted), how many commands run on
it now and how many wait for their turn, how many reads and writes
it has done and their octets. With no node connected, "running" and
"waiting" are 0. No action.

### UNITCFG

At start-up: a disk the server serves, its name on the OpenVMS
nodes ($42$DKA100, or MHUB$DKA100 with /ALLOCLASS=0), its file, its size and its MSCP unit number. No action.

### CONOPEN

An OpenVMS node has connected to the disk server (MSCP$DISK): its
class driver VMS$DISK_CL_DRVR. The message shows the credits on
both sides. No action.

### CONCLOSE

A connection to the disk server is closed: the node disconnected, or
its circuit closed, or the server ended it. Action: none, the node
connects again. If the reason is "an end message was held back", see
MSCPERR.

### MSCPERR

The server was about to send an answer that would crash the OpenVMS
node (a BUG_CHECK of its class driver). It did not send it and ended
the connection. This is an error of the server. Action: report it
with the log of /TRACE.

### IOERR

The Linux file of a disk could not be read or written: the message
shows the command, the disk, the block, the size and the error of
Linux. The OpenVMS node gets a drive error (SS$_DRVERR or similar).
Action: check the file system of the Linux host - "No space left on
device" means the file is sparse and the file system is full.

### BLKERR

A chunk of data came from a node for no command of the server: the
command is over, or the node sent it twice. The server drops it.
Action: none if it is rare. If it is often, report it with the log
of /TRACE.

### UNSAFE

/UNSAFE or /SYNC=NONE is given: a WRITE is answered before its data
is on disk. Action: do not use it with data you need.

### BKSTATS

One for each disk, written after its UNITSTATS: the sector of the
disk, how many writes were done as read-modify-write (a 4096-octet
sector), how many sectors they needed first, how many of those came
out of the cache of the 128 sectors last written instead of the disk,
and how many were among the last 2, 16, 128 and 512 written (what a
cache of that size would save), how many writes waited for another one on the same sectors,
with /SYNC=GROUP how many flushes covered how many writes, and how
many ERASE commands came (ZERO) and how many of them were done by
writing zeros to a file. No action.

### LATSTATS

One for each disk and each of READ and WRITE, written after its
BKSTATS, when the disk has had such commands: how many, the average
time from the command to its answer in microseconds, and what part
of it was the Linux disk (backend), what part the data on the LAN
(transfers) and the rest (waiting for the disk, the server itself).
Then the longest time and how many commands took under 0.1, 0.3, 1,
3 and 10 ms and longer. The counters start when the server starts:
to measure a test, send SIGUSR1 before and after it and subtract. No
action.

### IOSTUCK

A command has been in the Linux disk for 15 seconds. The server
cancels it; when the cancel takes, the node gets a drive error. When
it does not (Linux is still writing), the disk waits for it. Action:
check the Linux disk under the unit - a dead disk, a lost iSCSI or
NFS server. Look at dmesg.

### DEVBUSY

The server cannot open a block device of /UNITn for itself: it is
mounted on the Linux host, it is in use by LVM or RAID, or another
server has it. The server stops. Action: unmount it, or give another
device. Never serve a device the Linux host uses.

### NODIRECT

The file system of a file of /UNITn does not allow direct I/O
(O_DIRECT) - tmpfs of an old kernel, some network file systems - or
the size of the file is not whole blocks of 512 octets. The server
serves the file through the page cache: it works, but every write
costs more. No action; for speed put the image on ext4 or XFS, or
serve a partition or an LVM volume.

### MISALIGN

A block device of /UNITn - most often a partition - does not start
on a physical sector of the Linux disk: it starts that many octets
into one. Then no write of a node fills whole sectors, and every one
is a read-modify-write, in the server or in the disk itself. The
server still serves the device. Action: for a new disk, make the
partition start on a multiple of 1 MB (parted, fdisk do so by
default) and copy the data over.

### UPTIME

The first message of SIGUSR1: the version, the name of the server,
how long it has run and the zone of /TIMEZONE. No action.

### NODESTATS

One for each node the server knows, after UPTIME: the state of the
channel and how many times it has opened, the state of the virtual
circuit and how many times it has opened, the SCS connections open
now. "OPEN" twice and one connection is a node that uses the disks.
A count of opens that grows means the node comes and goes: see
CHCLOSE and VCCLOSE in the log. No action otherwise.

### SHUTDOWN

The server got SIGINT or SIGTERM and stops. It sends HOSTSHUT on
every open virtual circuit, then, if it has announced itself, BYE to
the group: the OpenVMS nodes close their circuits and channels to it
at once. No action.

### STATS

The counters: frames received (CC, TR, dropped, own, with a bad
password) and sent, send errors. Dropped frames are frames of other groups or for other
nodes; that is normal.

### NOTOPIC

/HELP names a topic the library does not have.

## Author

StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)
