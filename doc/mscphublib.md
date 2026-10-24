# MSCPHUB -- MSCP disk server for OpenVMS nodes

MSCPHUB runs on Linux. It lets the OpenVMS nodes on the same LAN use
Linux files and block devices as disks of their own. To an OpenVMS
node it is a disk controller of the cluster, like an HSC: the node
sees its disks with SHOW DEVICE and mounts them with MOUNT. Several
nodes can mount one disk with MOUNT/CLUSTER.

MSCPHUB speaks NISCA, the protocol of PEDRIVER, on the Ethernet. It is
not a member of the VMScluster and has no votes. Nothing is changed on
the OpenVMS nodes.

A WRITE is answered only when its data is on the disk of the Linux
computer: OpenVMS never asks a disk to flush a cache of its own.

Format:

```
mscphub /CONFIG=file-spec
mscphub /INTERFACE=name /NODE=name /SYSTEMID=n /GROUP=n /AUTHORIZE=hex [/qualifier...]
mscphub /HELP[="topic..."]
```

A qualifier is given on the command line or, one on a line, in the
file of /CONFIG. --NAME=value may be written for /NAME=value. A
qualifier the server does not know is passed over with a warning.

The manuals: the MSCPHUB User's Guide and the MSCPHUB Programmer's
Guide, in /opt/mscphub/share/doc/mscphub; the manual page mscphub(8).

## /ACK -- when the server acknowledges alone

```
/ACK=HOLD | FLUSH     (default HOLD)
```

Specifies when the server acknowledges the packets of a node in a
packet of its own. Most of the time the acknowledgement travels in a
packet of the server - the answer to a command, the data of a READ.
When the server has nothing to send, it sends the acknowledgement
alone.

- HOLD -- alone only when the node asks for it at once, after 4
  packets, or after 20 ms at most; otherwise the acknowledgement
  waits for the answer of the server.
- FLUSH -- alone as soon as the server has taken all the frames that
  came in.

The default is /ACK=HOLD. HOLD sends far fewer packets; the messages
ACKSTATS and LATSTATS compare the two.

## /ALLOCLASS -- the allocation class of the disks

```
/ALLOCLASS=n     (default 0)
```

Specifies the allocation class of the server, 0 to 255. With
/ALLOCLASS=42 the OpenVMS nodes name the disk DKA100 of the server
$42$DKA100; with 0 they name it MHUB$DKA100, after the SCSNODE of the
server. The default is 0.

Do not change it while OpenVMS nodes use the disks. Do not give a
class that the cluster uses for disks of the same names.

## /AUTHORIZE -- the cluster password

```
/AUTHORIZE=hex
```

Specifies the cluster password, as the 8-octet hash OpenVMS keeps in
SYS$SYSTEM:CLUSTER_AUTHORIZE.DAT: 16 hexadecimal digits, the octets
from the lowest one. Required. To find it, dump the file on an
OpenVMS node:

```
$ DUMP SYS$SYSTEM:CLUSTER_AUTHORIZE.DAT
```

The hash is the quadword at offset 8. The longwords 00000000 8EF54F24
there give /AUTHORIZE=5A3C9E1700000000.

## /CONFIG -- read the qualifiers from a file

```
/CONFIG=file-spec
```

Reads the qualifiers from the file named, one on a line, as on the
command line. A line that begins with ! is a comment. The qualifiers
of the command line are taken too.

```
! MSCPHUB of our cluster
/INTERFACE=mh0
/NODE=MHUB
/SYSTEMID=65200
/GROUP=4242
/AUTHORIZE=5A3C9E1700000000
```

## /CREDITS -- commands a node may send at once

```
/CREDITS=n     (default 16)
```

Specifies how many commands each OpenVMS node may have in progress on
the disk server at a time, 2 to 64. The value is given to a node when
it connects. The default is 16.

## /GROUP -- the cluster group number

```
/GROUP=n
```

Specifies the cluster group number of the OpenVMS nodes, 1 to 65279.
Required. On OpenVMS, MCR SYSMAN CONFIGURATION SHOW
CLUSTER_AUTHORIZATION shows it.

## /HELP -- display this description

```
/HELP
/HELP="topic subtopic"
```

Displays the help library of the server: the whole description, or
the topic named, and stops.

```
$ mscphub /HELP=/SYSTEMID
$ mscphub "/HELP=Messages IDCLASH"
```

## /INTERFACE -- the LAN interface

```
/INTERFACE=name
```

Specifies the Linux interface the OpenVMS nodes are on: an Ethernet
adapter, a bridge, a VLAN, a tap device in a bridge. Required. The
server needs CAP_NET_RAW: run it as root. The MAC address of the
interface is not changed.

## /LISTEN -- seconds of silence at start-up

```
/LISTEN=seconds     (default 4)
```

Specifies how long the server listens before it sends anything, 1 to
60 seconds. In that time it learns the nodes of the LAN from their
HELLO messages. A node that uses the name or the system ID of the
server stops it. The default is 4.

## /LOGFILE -- the log file

```
/LOGFILE=file-spec
```

Writes the messages to the file named. By default they go to the
standard error, which systemd keeps in its journal; the scripts of
OpenRC and System V init send it to /var/log/mscphub.log.

## /MODBUS -- serve the counters over Modbus/TCP

```
/MODBUS=[address:]port
```

Specifies the TCP port, and the address, on which the server answers
Modbus/TCP requests for its counters: a SCADA, a PLC panel or any
Modbus client can read them. Port 502 is the Modbus port; any other
will do. Without /MODBUS nothing is served.

The counters are served by a thread of their own, out of a copy the
server makes every 100 ms; a client never holds up the disks. The
registers are read with the functions 03 and 04, the identification
of the device with 2B/0E; nothing can be written. The map of the
registers is in the User's Guide, Appendix D.

```
/MODBUS=502
/MODBUS=192.168.1.10:1502
```

## /NODE -- the SCSNODE of the server

```
/NODE=name
```

Specifies the SCSNODE of the server, 1 to 6 of the characters A-Z,
0-9, $ and _. Required. It must not be the name of any other node of
the group.

## /PASSIVE -- never transmit

```
/PASSIVE
```

The server only listens and reports the nodes it sees. It opens no
channel and sends nothing at all. Safe on any LAN, for example to
look at a production cluster.

## /SYNC -- when a WRITE is on disk

```
/SYNC=DSYNC | GROUP | NONE     (default DSYNC)
```

Specifies how a WRITE is made durable before it is answered.

- DSYNC -- every WRITE is written to the medium by itself.
- GROUP -- the writes that end while the disk is being flushed wait
  for the next flush and are answered together: fewer flushes when
  many nodes write at once. As safe as DSYNC.
- NONE -- the same as /UNSAFE.

The default is /SYNC=DSYNC.

## /SYSTEMID -- the SCSSYSTEMID of the server

```
/SYSTEMID=n
```

Specifies the SCSSYSTEMID of the server, 1 to 65535. Required. It
must differ from the SCSSYSTEMID of every other node of the group: on
the LAN the nodes are told apart by the low 16 bits of the
SCSSYSTEMID only. On OpenVMS, MCR SYSGEN SHOW SCSSYSTEMID and SHOW
CLUSTER show the IDs in use.

## /TIMEZONE -- the zone of the time told to the nodes

```
/TIMEZONE=LOCAL | UTC | [+|-]hh[:mm]     (default LOCAL)
```

Specifies the zone of the time the server tells the nodes when it
opens a circuit. SHOW CLUSTER and SCACP show it as the time the server
came up. LOCAL is the zone of the Linux computer, summer time and
all. The default is LOCAL. Only the operator sees this time; the
protocol works with any zone.

## /TRACE -- log every frame

```
/TRACE
```

Writes every frame received and every step of every channel to the
log. Use it for a short time only.

## /UNIT0 ... /UNIT7 -- the disks

```
/UNIT0=name,path[,RO][,MEDIA=medium][,SECTOR=octets]
```

Specifies a disk. The name is DKcn: a controller letter A to Z and a
number 0 to 9999, for example DKA100. The path is:

- a Linux file. Its size at start-up, in blocks of 512 octets, is the
  size of the disk. The server takes the file for itself with a lock:
  a second server cannot serve it too. It reads and writes the file
  with direct I/O when the file system allows it, else through the
  page cache (NODIRECT).
- a block device: a disk, a partition, an LVM volume, a loop device,
  an iSCSI LUN. The server opens it for exclusive use: a device that
  is mounted on the Linux computer, or that another server has, is
  refused (DEVBUSY). A partition that does not start on a physical
  sector is warned of (MISALIGN).

The options, in any order:

- RO -- read only. The node mounts the disk write-locked.
- MEDIA=medium -- what the node is told the disk is: two or three
  letters and a number. The default is RZ29, and RRD42 with RO.
- SECTOR=octets -- takes a file as a disk of 512, 1024, 2048 or 4096
  octet sectors. For tests of 4K disks only; a block device tells its
  sector itself.

A device of 4096-octet sectors works: a write that does not fill whole
sectors is done as read-modify-write. OpenVMS writes in blocks of 512
octets. To have most of its writes fill whole sectors, INITIALIZE the
disk with a cluster of 8 or a multiple of 8:
INITIALIZE/CLUSTER=16 $42$DKA100: DATA1. OpenVMS does so by itself on
a disk of more than 50000 blocks; on a smaller one it takes 1.

Examples:

```
/ALLOCLASS=42
/UNIT0=DKA100,/srv/vms/data1.img
/UNIT1=DKA200,/dev/vg0/vmsdata
/UNIT2=DKA300,/srv/iso/axpvms083.iso,RO
```

## /UNSAFE -- answer a WRITE before its data is on disk

```
/UNSAFE
```

Answers a WRITE as soon as Linux has taken the data, before it is on
the disk. Faster, but a crash or a power failure of the Linux computer
can lose data an OpenVMS node believes is written. For tests only. The
server says UNSAFE when it starts.

## MHMON -- the monitor of the server

```
mhmon [DISK | NODES | SERVER | ALL] [/qualifier...]
```

MHMON shows what a running server does, as the MONITOR utility of
OpenVMS does: every /INTERVAL seconds, the value of the last interval
(CUR), and the average, the least and the greatest since MHMON
started (AVE, MIN, MAX). It reads a copy of the counters the server
makes every 100 ms in shared memory (/dev/shm/mscphub.<node>), and
never writes to it. With /RECORD it writes the counters to a file of
the T4 style, for TLViz and other tools of T4.

Run it as root, on the computer of the server.

```
# mhmon
# mhmon NODES
# mhmon DISK /ITEM=ALL /INTERVAL=5
# mhmon ALL /NODISPLAY /RECORD=/var/opt/mscphub/t4 /INTERVAL=60
```

### Classes

- DISK -- every disk: its name on the nodes, the label of its volume,
  the nodes that have it online (mounted, or being mounted), and the
  item of /ITEM. The default.
- NODES -- every node the server knows: its system ID, the state of
  its channel and of its virtual circuit, how many times the channel
  has opened, its connections, the disks online to it and the MSCP
  commands it sends a second.
- SERVER -- frames, kilobytes, retransmissions, duplicate and out of
  order packets, data sent and taken, commands in progress, the data
  buffers in use.
- ALL -- the three in turn, one an interval.

### /ENDING

```
/ENDING=hh:mm[:ss]
```

Specifies when MHMON stops: today, or tomorrow when the time is past.
Without it MHMON runs until Ctrl/C or SIGTERM.

### /INTERVAL

```
/INTERVAL=seconds     (default 3)
```

Specifies the seconds between two samples, 1 to 3600. The samples
fall on the boundaries of the interval of the clock: with
/INTERVAL=60, on the minute.

### /ITEM

```
/ITEM=OPERATION_RATE | THROUGHPUT | QUEUE_LENGTH | RESPONSE_TIME | ALL
```

Specifies what the class DISK shows: commands a second, kilobytes a
second, the average number of commands running and waiting, the
average time from a command to its answer in milliseconds; ALL shows
them in turn. The default is OPERATION_RATE.

### /NODE

```
/NODE=name
```

Specifies the SCSNODE of the server to watch, when several servers
run on the computer. Without it MHMON takes the only one there is.

### /NODISPLAY

```
/NODISPLAY
```

Nothing is shown: MHMON only records. /RECORD is required with it.

### /RECORD

```
/RECORD=file-spec | directory
```

Writes the rates of every interval to a file of the T4 style: the
node, the date and the time of the start, the names of the columns,
then a row a sample - the time of the end of the sample, then the
values. A field with no value is empty; every row has the same number
of fields. The columns are those of the server when the file is
begun: its rows, four items of every disk, the commands of every
node.

A directory gets a file a day,
T4_<node>_<date>_<start>_<end>.CSV, the form of the files of T4; the
file being written ends in _RUN. A recording waits for a server that
does not run yet, and goes on when it comes back.

### T4 -- record all the time

The unit mscphub-t4.service records the counters every minute to
/var/opt/mscphub/t4, a file a day. It is installed but not enabled:

```
# systemctl enable --now mscphub-t4
```

## Signals

```
SIGINT, SIGTERM    stop the server (SHUTDOWN)
SIGUSR1            write the state and the counters to the log: UPTIME,
                   a NODESTATS for each node, STATS, TRSTATS, ACKSTATS,
                   BLKSTATS, MSCPSTATS, then UNITSTATS, BKSTATS and
                   LATSTATS for each disk
```

Under systemd:

```
# systemctl kill -s USR1 mscphub
# journalctl -u mscphub -n 40
```

The counters run from the start of the server. To measure a test,
send SIGUSR1 before and after it and subtract.

## Service -- run the server under systemd

The installation puts the unit mscphub.service in place, but does not
enable it: a server that starts by itself talks to the cluster on its
LAN at once. The unit reads /etc/mscphub/mscphub.conf and does
not start without it.

```
# cp /etc/mscphub/mscphub.conf.example /etc/mscphub/mscphub.conf
# vi /etc/mscphub/mscphub.conf
# systemctl enable --now mscphub
# journalctl -u mscphub -f
```

To pin the server to a CPU, or to change its start line, use a
drop-in (systemctl edit mscphub); do not edit the unit itself.

Without systemd the installation puts a script of OpenRC or of
System V init in /etc/init.d instead, not enabled either; its log is
/var/log/mscphub.log:

```
# rc-update add mscphub default         OpenRC
# rc-service mscphub start
# rc-service mscphub report             SIGUSR1

# update-rc.d mscphub defaults          System V init
# /etc/init.d/mscphub start
# /etc/init.d/mscphub report            SIGUSR1
```

Its variables - the parameter file, the log, the CPUs - are set in
/etc/conf.d/mscphub (OpenRC) or /etc/default/mscphub (System V).

## Troubleshooting -- what to check first

The server stops at once. The last message is of severity F and says
why. See the topic Messages.

No node is seen: ANNOUNCE says 0 other nodes. Check /GROUP. Check that
the interface is up and in the right bridge or VLAN. On OpenVMS, check
that PEDRIVER runs: MCR SCACP SHOW CHANNEL. Look at the wire:

```
# tcpdump -i eth1 ether proto 0x6007
```

The nodes are seen, but no CHOPEN comes. Look for AUTHFAIL: the hash
of /AUTHORIZE is not that of the group. Check that /PASSIVE is not
given.

A channel opens and closes again and again. Read the reason in
CHCLOSE. "nothing heard from the node" means that frames are lost
between the node and the server: check the switch, the bridge, the
VLAN.

The channel is open, but no VCOPEN comes. Run the server with /TRACE
for a short time and look for "PPD START", and for a warning after it.

The circuit is open, but the disks do not appear on the node. Look
for CONOPEN. No CONOPEN: check that /UNITn are given (UNITCFG at
start-up). On OpenVMS, check that the process CONFIGURE runs (SHOW
SYSTEM).

The circuit closes again and again, or writes are slow and reads are
not. Read the reason in VCCLOSE, and see TRSTATS: many retransmissions
mean frames to the nodes are lost, many "out of order" mean frames
from the nodes are lost.

MOUNT or COPY fails with an I/O error on the node. Look for IOERR,
IOSTUCK, MSCPERR and BLKERR.

The disks are slow. Look for NODIRECT and MISALIGN. A sparse image is
slow until it has been written once; write it full with dd. LATSTATS
shows where the time of a command goes.

## Messages -- the conditions the server signals

Every message has the OpenVMS form:

```
%MSCPHUB-F-IDCLASH, node ALPHA1 (08-00-2B-E5-40-01) uses the system ID low word 65534 of this server
```

The letter after MSCPHUB is the severity: S success, I information, W
warning, E error, F fatal (the server stops). In the texts below, a
word in quotes stands for a value the server puts in.

### ACKSTATS

```
acknowledgements (/ACK='mode'): 'n' asked for at once; 'n' with a packet of ours, 'n' alone: 'n' asked, 'n' for a duplicate, 'n' for 'n' packets, 'n' when drained, 'n' on the tick
```

Severity: information.

Explanation: Written on SIGUSR1 after TRSTATS. How many packets of the
nodes asked to be acknowledged at once, how many acknowledgements went
in a packet of the server, and how many went alone - because the node
asked, for a packet that came twice, for a batch of packets
(/ACK=HOLD), when all frames were taken (/ACK=FLUSH), or on the 20 ms
tick. Fewer alone is better.

User Action: None.

### ANNOUNCE

```
node 'name' 'state' on 'interface', 'n' other node(s) seen in group 'n'
```

Severity: information.

Explanation: The listen time is over and the server now sends HELLO
messages. With /PASSIVE it says "listens only" and sends nothing.

User Action: None. If no other node is seen, see Troubleshooting.

### AUTHFAIL

```
node 'name' ('systemid', 'adapter') sent a CC message with a wrong cluster password, ignored
```

Severity: warning.

Explanation: The node is in the group of the server, but its password
hash is not that of /AUTHORIZE. No channel is opened to the node. The
message is written once for each node.

User Action: Give /AUTHORIZE the hash of the cluster of this node (see
/AUTHORIZE), and check /GROUP.

### BKSTATS

```
unit 'name': sector 'n', 'n' read-modify-write(s) needing 'n' sector(s), 'n' from the cache (in the last 2/16/128/512 written: 'n'/'n'/'n'/'n'), 'n' wait(s) for a range, 'n' group fdatasync(s) for 'n' write(s), 'n' ZERO(s) ('n' as zeros written)
```

Severity: information.

Explanation: Written on SIGUSR1 for each disk, after its UNITSTATS:
the sector of the disk; how many writes were done as read-modify-write
(a disk of 4096-octet sectors) and how many sectors they needed first;
how many of those came from the cache of the 128 sectors last written;
how many were among the last 2, 16, 128 and 512 written; how many
writes waited for another one on the same sectors; with /SYNC=GROUP,
how many flushes covered how many writes; how many ERASE commands
came, and how many of them were done by writing zeros.

User Action: None.

### BLKERR

```
'operation' from 'name' ('systemid'), XCT_ID 'id': 'reason', dropped
```

Severity: warning.

Explanation: A chunk of data came from a node for no command of the
server: the command is over, or the node sent the chunk twice. The
server drops it. It does no harm to either side.

User Action: None if it is rare. If it is frequent, report it with a
log of /TRACE.

### BLKSTATS

```
block transfers: 'n' chunk(s) sent ('n' octets), 'n' RET_CNF, 'n' REQ_DAT, 'n' chunk(s) taken ('n' octets), 'n' dropped
```

Severity: information.

Explanation: Written on SIGUSR1 after ACKSTATS: the chunks of data
sent to the nodes (READ) and their octets, the confirmations the nodes
sent for them, the requests for data the server sent (WRITE), the
chunks taken in and their octets, the chunks dropped.

User Action: None.

### CHCLOSE

```
channel to node 'name' ('systemid') is closed: 'reason'
```

Severity: warning.

Explanation: The channel to a node is closed. The reason is one of:

```
BYE received, the node is going down     the node shuts down or crashes
restarted by the node                    the node started a new channel
no answer to the handshake in 5 seconds
nothing heard from the node for 8 seconds
the node has changed its name or system ID
the server is stopping
```

A node that is still up opens the channel again in a few seconds.

User Action: None if the channel opens again. If it does not, or if it
closes again and again, see Troubleshooting.

### CHOPEN

```
channel to node 'name' ('systemid') is open, CC V1.'n', pipe quota 'n', buffer 'n', services 'xxxx'/'xxxx'
```

Severity: information.

Explanation: The handshake with the node is over. The message shows
the version of the node, its pipe quota, its buffer size and its
services (offered/required).

User Action: None.

### CONCLOSE

```
connection 'name' of 'node' ('systemid') is closed: 'reason'
```

Severity: information.

Explanation: A connection to the disk server is closed: the node
disconnected, its circuit closed, or the server ended it.

User Action: None; the node connects again. If the reason is "an end
message was held back", see MSCPERR.

### CONOPEN

```
connection 'name' of 'node' ('systemid') to 'server' is open, credit 'n'/'n'
```

Severity: information.

Explanation: An OpenVMS node has connected to the disk server
MSCP$DISK: its disk class driver, VMS$DISK_CL_DRVR. The message shows
the credits of both sides. The disks of the server now appear on the
node.

User Action: None.

### DEVBUSY

```
the device 'device' is in use: mounted on this host, or another server has it
```

Severity: fatal.

Explanation: The server cannot open a block device of /UNITn for
itself: it is mounted on the Linux computer, it is in use by LVM or
RAID, or another server has it. The server stops.

User Action: Dismount the device, or give another one. Never serve a
device the Linux computer uses.

### IDCLASH

```
node 'name' ('adapter') uses the system ID low word 'n' of this server
```

Severity: fatal.

Explanation: Another node uses the same low 16 bits of the SCSSYSTEMID
as the server. Two such nodes cannot be told apart on the LAN. The
server stops and sends nothing.

User Action: Give the server another /SYSTEMID. On OpenVMS, SHOW
CLUSTER or MCR SYSGEN SHOW SCSSYSTEMID shows the IDs in use.

### IOERR

```
'operation' of unit 'name', LBN 'n', 'n' octets, failed: errno='n' ('text')
```

Severity: error.

Explanation: The Linux file or device of a disk could not be read or
written. The message shows the command, the disk, the block, the size
and the error of Linux. The OpenVMS node gets a drive error.

User Action: Check the file system and the disk of the Linux computer.
"No space left on device" means that the file is sparse and its file
system is full.

### IOSTUCK

```
'operation' of unit 'name', LBN 'n', is in the backend for 'n' seconds: cancelled
```

Severity: warning.

Explanation: A command has been in the Linux disk for 15 seconds. The
server cancels it; when the cancel takes, the node gets a drive error.
When it does not - Linux is still writing - the disk waits for it.

User Action: Check the Linux disk under the unit: a dead disk, a lost
iSCSI or NFS server. Look at dmesg.

### IVPARAM

```
illegal value 'value' of /'name': 'what is expected'
```

Severity: fatal.

Explanation: A qualifier has a wrong value. The message says what is
expected. The server stops.

User Action: Correct the value.

### JUMBO

```
'interface' has an MTU of 'n': a packet of a block transfer carries up to 'octets' octets of data
```

Severity: information.

Explanation: The MTU of the interface is larger than 1500: the server
offers the nodes jumbo frames. An MTU over 9000 is held to 9000, and the
message says so. A node uses them only when its own adapter has jumbo
frames on (LAN_FLAGS bit 6) and NISCS_MAX_PKTSZ allows them.

User Action: None, if every switch and tunnel on the way carries jumbo
frames. If not, set the MTU back to 1500 and start the server again: a
packet too long for the path is lost and the virtual circuit fails.

### LATSTATS

```
unit 'name', 'operation': 'n' command(s), average 'n' us (backend 'n', transfers 'n', the rest 'n'), longest 'n' us; under 0.1/0.3/1/3/10 ms and longer: 'n'/'n'/'n'/'n'/'n'/'n'
```

Severity: information.

Explanation: Written on SIGUSR1 for each disk and for each of READ and
WRITE, after its BKSTATS, when the disk has had such commands. The
average time from a command to its answer, in microseconds, and how it
was spent: the Linux disk (backend), the data on the LAN (transfers),
and the rest - waiting for the disk, the server itself. Then the
longest time, and how many commands took under 0.1, 0.3, 1, 3 and 10
ms and longer.

User Action: None.

### LISTEN

```
listening 'n' second(s) before announcing the node
```

Severity: information.

Explanation: The server listens for /LISTEN seconds before it sends
anything.

User Action: None.

### MANYNODES

```
more than 'n' nodes on the LAN, node 'name' is not remembered
```

Severity: warning.

Explanation: There are more nodes on the LAN than the server remembers
(64). A node beyond that is not checked for a clash.

User Action: Report it.

### MANYSERVERS

```
'n' MSCPHUB servers run on this host: give /NODE
```

Severity: error.

Explanation: MHMON has found the statistics of several servers on the
computer and does not know which one to show.

User Action: Give MHMON the SCSNODE of the server: /NODE=name.

### MBUSERR

```
the counters are not served over Modbus/TCP: 'call' failed, errno='n' ('text')
```

Severity: warning.

Explanation: /MODBUS is given, but the server cannot serve the
counters there: most often the port is in use by another program
(bind, errno=98) or the address is not of this computer (bind,
errno=99). The server serves its disks without it.

User Action: Give another port or address in /MODBUS, or stop the
program that has the port.

### MISALIGN

```
'device' starts 'n' octets into a physical sector of 'n': every write is a read-modify-write of the disk
```

Severity: warning.

Explanation: A block device of /UNITn - most often a partition - does
not start on a physical sector of the Linux disk. No write of a node
then fills whole sectors, and every one is a read-modify-write. The
server still serves the device.

User Action: For a new disk, make the partition start on a multiple of
1 MB (parted and fdisk do so by default) and copy the data over.

### MODBUS

```
the counters are served over Modbus/TCP on 'address', port 'n'
```

Severity: information.

Explanation: The server answers Modbus/TCP requests for its counters
(/MODBUS).

User Action: None.

### MSCPERR

```
end message of 'operation' to 'connection' of 'node' ('systemid') held back: 'reason'
```

Severity: error.

Explanation: The server held back an answer
that would crash the OpenVMS node, and ended the connection. This is
an error of the server.

User Action: Report it, with a log of /TRACE.

### MSCPSTATS

```
MSCP server: 'n' command(s) in progress, 'n' octets of data buffers in use
```

Severity: information.

Explanation: Written on SIGUSR1 after BLKSTATS. When no node uses the
disks, both numbers are 0.

User Action: None. A number that stays above 0 with no node connected
is a fault of the server: report it with the log.

### NAMECLASH

```
node 'name' ('adapter', system ID 'n') uses the SCSNODE of this server
```

Severity: fatal.

Explanation: Another node has the same SCSNODE as the server. The
server stops and sends nothing.

User Action: Give the server another /NODE.

### NEWNODE

```
node 'name', system ID 'n', adapter 'adapter', CC V1.'n' ('n' octets)
```

Severity: information.

Explanation: The server sees a node for the first time: its name, its
system ID, its adapter and its version.

User Action: None.

### NODESTATS

```
node 'name' ('systemid'): channel 'state', open 'n' time(s); virtual circuit 'state', open 'n' time(s); 'n' connection(s) open
```

Severity: information.

Explanation: Written on SIGUSR1 for each node the server knows, after
UPTIME. OPEN twice and one connection is a node that uses the disks. A
count of opens that grows means that the node comes and goes.

User Action: None. If the counts grow, see CHCLOSE and VCCLOSE in the
log.

### NODIRECT

```
'path': no direct I/O ('reason'), served through the page cache
```

Severity: information.

Explanation: The file system of a file of /UNITn does not allow direct
I/O, or the size of the file is not whole blocks of 512 octets. The
server serves the file through the page cache: it works, but every
write costs more.

User Action: None; for speed, put the image on ext4 or XFS, or serve a
partition or an LVM volume.

### NOPARAM

```
the parameter /'name' is mandatory
```

Severity: fatal.

Explanation: A required qualifier is missing. The server stops.

User Action: Add it.

### NOSERVER

```
no MSCPHUB server runs on this host'text'
```

Severity: error.

Explanation: MHMON has found no statistics of a server, or the server
does not update them - MHMON watches them for a quarter of a second
when it starts. No server runs on this computer, or not the one /NODE
names, or it hangs, or MHMON is not run as root. With /RECORD, MHMON
waits for the server.

User Action: Start the server (systemctl start mscphub); check /NODE.

### NOSTAT

```
no statistics for MHMON: 'call' failed, errno='n' ('text')
```

Severity: warning.

Explanation: The server cannot make its statistics block in shared
memory (/dev/shm). It serves its disks, but MHMON and /MODBUS see
nothing.

User Action: Check that /dev/shm is there and is not full (df
/dev/shm).

### NOTOPIC

```
sorry, no documentation on 'topic'
```

Severity: error.

Explanation: /HELP names a topic the help library does not have.

User Action: Give mscphub /HELP to see the topics.

### RCVBUF

```
the socket of 'interface' holds 'n' octets of frames, not 'n': a burst of jumbo frames can be lost; raise net.core.rmem_max to 'n'
```

Severity: warning.

Explanation: With jumbo frames the server asked for a receive buffer of
4 MB and got less: the kernel holds it to net.core.rmem_max. In a burst
from several nodes the frames beyond the buffer are lost, and the nodes
send them again: the disks work, more slowly.

User Action: Raise net.core.rmem_max to the value the message gives
(sysctl, and a file in /etc/sysctl.d) and start the server again.

### RECORD

```
recording every 'n' second(s) to 'file'
```

Severity: information.

Explanation: MHMON has begun a file of /RECORD.

User Action: None.

### SHUTDOWN

```
signal 'n' received, the server is stopping
```

Severity: information.

Explanation: The server got SIGINT or SIGTERM. It sends HOSTSHUT on
every open virtual circuit and, if it has announced itself, BYE to the
group: the OpenVMS nodes close their circuits and channels to it at
once.

User Action: None.

### SOLICIT

```
satellite 'name' ('n') asks for its system disk 'disk' - 'outcome'
```

Severity: information.

Explanation: The loader of a satellite looks for the server of its
system disk. The outcome is "served" (the disk is one of this server,
which answers), "not served here" (another server may serve it), or
"not served: the unit is read only" (a system disk must be writable).
The message comes once for a boot of a node.

User Action: None when the disk is served. When this server should
serve it, compare the name with the names in UNITCFG: the allocation
class and the name of the unit must be the same. Serve a system disk
writable.

### STARTUP

```
MSCPHUB 'ident', node 'name', system ID 'n', group 'n', on 'interface' ('address')
```

Severity: information.

Explanation: The server has started.

User Action: None.

### STATS

```
rx 'n' frame(s) ('n' CC, 'n' TR, 'n' dropped, 'n' own, 'n' bad password), tx 'n' frame(s), 'n' error(s)
```

Severity: information.

Explanation: Written on SIGUSR1 after the NODESTATS: frames received
and sent, and send errors. Dropped frames are frames of other groups
or for other nodes; that is normal.

User Action: None.

### STATVERS

```
the statistics of 'name' are of version 'n', MHMON reads version 'n'
```

Severity: error.

Explanation: The server and MHMON are of different versions, and the
layout of the statistics has changed between them.

User Action: Use the MHMON installed with the server; install both
from one kit.

### SYSERR

```
'call' failed, errno='n' ('text')
```

Severity: fatal.

Explanation: A Linux system call failed. The server stops. The most
frequent ones:

```
socket(AF_PACKET), errno=1      the server is not run as root (CAP_NET_RAW)
ioctl(SIOCGIFINDEX), errno=19   there is no such interface
```

User Action: Correct what the call and errno name.

### TRSTATS

```
transport: 'n' packet(s) sent, 'n' retransmitted, 'n' received, 'n' duplicate(s), 'n' out of order, 'n' of an old channel; window up to 'n'
```

Severity: information.

Explanation: Written on SIGUSR1 after STATS: packets sent,
retransmitted, received, duplicates, out of order, packets of an old
channel, and the widest the send window has been.

User Action: None. A few retransmissions are normal; many "out of
order" mean that frames from the nodes are lost (see Troubleshooting).

### UNITCFG

```
unit 'name' is 'path', 'n' block(s)'attributes', SLUN 'xxxx'
```

Severity: information.

Explanation: At start-up, for every disk: its name on the OpenVMS
nodes, its file or device, its size, its kind, sector and medium, and
its MSCP unit number.

User Action: None.

### UNITSTATS

```
unit 'name': ONLINE to 'n' host(s), 'n' command(s) running, 'n' waiting, 'n' read(s) ('n' octets), 'n' write(s) ('n' octets)
```

Severity: information.

Explanation: Written on SIGUSR1 for each disk, after MSCPSTATS: to how
many nodes the disk is online (mounted, or being mounted), how many
commands run on it and how many wait for their turn, how many reads
and writes it has done.

User Action: None.

### UNSAFE

```
/SYNC=NONE: a WRITE is answered before its data is on disk - for tests only
```

Severity: warning.

Explanation: /UNSAFE or /SYNC=NONE is given.

User Action: Do not use it with data you need.

### UPTIME

```
MSCPHUB 'ident', node 'name', up 'n' day(s) 'hh':'mm':'ss', OpenVMS time of 'zone'
```

Severity: information.

Explanation: The first message on SIGUSR1: the version, the name of
the server, how long it has run and the zone of /TIMEZONE.

User Action: None.

### VCCLOSE

```
virtual circuit to node 'name' ('systemid') is closed: 'reason'
```

Severity: warning.

Explanation: The virtual circuit to a node is closed. The reason is
one of:

```
the channel is closed ...                        see CHCLOSE
restarted by the node (START on the open path)   the node rebooted
HOSTSHUT received, the node is shutting down
no acknowledgement after 30 retransmissions      frames are lost
a block transfer the server does not take
the server is stopping
```

After a failure the server restarts the channel at once, and the node
opens the circuit again in a few seconds.

User Action: None if the circuit opens again. If it closes again and
again, see Troubleshooting.

### VCOPEN

```
virtual circuit to node 'name' ('systemid') is open: 'software' 'version' on 'hardware', datagram 'n', message 'n'
```

Severity: information.

Explanation: The node has answered the server, and SHOW CLUSTER on it
now shows the server. The message shows what the node told of itself:
its software, version and hardware, its largest datagram and message.

User Action: None.

## Author

StarLet Squad and Ruslan R. Laishev
