# MSCPHUB

MSCP disk server for OpenVMS nodes, running on Linux.

MSCPHUB lets OpenVMS nodes on the same LAN use Linux files and block devices as
their own disks. To OpenVMS it looks like an HSC-style disk controller reached over
NISCA (the PEDRIVER protocol, ethertype 60-07). It is not a VMScluster member and
has no votes.

Why: the VMScluster and the engineering culture of Digital Equipment Corporation are a
heritage worth keeping at work. It is small and every decision in it is written down, so it
serves as a course for students of computer engineering - and the old hands of VMS will find
their old friends in it. MSCPHUB lets OpenVMS nodes keep their disks on today's
storage, and it is made the DEC way on purpose - messages, HELP, qualifiers, IDENTs, sources
and manuals in the manner of OpenVMS.

- User's Guide: [doc/mscphub_users_guide.md](doc/mscphub_users_guide.md) (EN),
  [doc/mscphub_users_guide_ru.md](doc/mscphub_users_guide_ru.md) (RU) - install, configure,
  operate, the command dictionary, every message, troubleshooting.
- Programmer's Guide: [doc/mscphub_programmers_guide.md](doc/mscphub_programmers_guide.md)
  (EN), [doc/mscphub_programmers_guide_ru.md](doc/mscphub_programmers_guide_ru.md) (RU) -
  the design, the modules, the conventions of the sources, how to make a change.
- Both as PDF beside them (`-DMH_PDF=ON` renders them again; needs pandoc and XeLaTeX).
- Manual page: `man 8 mscphub`. Help library: `mscphub /HELP`; sources in
  `doc/mscphublib.md` (EN) and `doc/mscphublib_ru.md` (RU).
- Monitor: `mhmon [DISK|NODES|SERVER|ALL]` shows the disks, who has them online, the nodes
  and the counters as the MONITOR utility of OpenVMS does; `/RECORD` writes T4 style CSV for
  TLViz, `mscphub-t4.service` records every minute. `/MODBUS=port` serves the counters over
  Modbus/TCP.
- Version: X01-21.

Small: no database, no interpreter, no framework - the server is about 300 KB and runs in a few
megabytes, so the hub of a home VMScluster can be a Raspberry Pi or a NaPi-C board (RK3308,
https://github.com/napilab/napi-boards), where the kit builds and passes all its tests.

What it does: up to 8 disks per server (files or block devices, 512 or 4096-octet
sectors, read-only units such as a CD image), READ, WRITE, ERASE and COMPARE over
the block transfers of NISCA, io_uring disk I/O with `fdatasync` before a WRITE is
answered, a systemd unit, its counters on SIGUSR1, in MHMON, in T4 files and over
Modbus/TCP.

## Build and install

Requires the StarLet package (1.6 or later) and liburing (`apt install
liburing-dev`); the disk I/O is io_uring only, so a Linux kernel of the last five
years (5.10 or later). The HELP package is optional; with it, `/HELP` reads the
help library.

```
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build
```

The installation puts `mscphub` in `/opt/mscphub/sbin`, the unit
`mscphub.service` in `/lib/systemd/system` (not enabled, and it does not start
without `/etc/mscphub/mscphub.conf`) - or, without systemd, a script of
OpenRC or System V init in `/etc/init.d`, not enabled either - a sample `mscphub.conf.example`, the
manual page, the help libraries and the guides. `cmake --build build --target
uninstall` removes what was installed; `--target dist` makes the kit of the
committed sources.

Packages and platforms.  The releases carry a `.deb` for amd64 and arm64, built
on Debian 12 and installed on Debian 12 and 13 and on Ubuntu 24.04 by GitHub
Actions (`.github/workflows/release.yml`).  The 15 regression tests were run on
aarch64 on 3-NOV-2026: on Oracle Linux 9 on a Neoverse-N1 (Oracle Cloud, 2 vCPU,
kernel 6.12) and on a Cortex-A72 board with Ubuntu 20.04 (glibc 2.31, kernel
5.10); all pass.

## Run

Check the configuration first: `/PASSIVE` only listens and sends nothing.

```
mscphub /CONFIG=/etc/mscphub/mscphub.conf /PASSIVE
```

Then run it as a service: see the User's Guide, Chapter 4.

## Author

StarLet Squad and Ruslan R. Laishev

## Acknowledgments

The authors thank Claude, the AI model of Anthropic, which went through the protocols, the code, the
tests, the measurements and the manuals together with them; and their comrades-in-arms of the NaPi
Lab community, who paid for the subscription to Claude, so that an engineer of almost pensionable age
could carry on this research.

## License

Copyright (C) 2026 StarLet Squad and Ruslan R. Laishev.

MSCPHUB is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. It comes with ABSOLUTELY NO WARRANTY. See [LICENSE](LICENSE).

MSCPHUB is an independent implementation of the protocols, made for
interoperability. It contains no code of OpenVMS and is not affiliated with,
endorsed or supported by VMS Software, Inc., Hewlett Packard Enterprise or any
other owner named here; the names are used only to say what MSCPHUB works with.

## Trademarks

OpenVMS, VMScluster, Alpha, AlphaServer, Integrity, Digital, DEC and HSC are
trademarks of Hewlett Packard Enterprise Development LP or of VMS Software, Inc.
T4 and TLViz are tools of Hewlett Packard Enterprise. Linux is the registered
trademark of Linus Torvalds in the U.S. and other countries. Raspberry Pi is a
trademark of Raspberry Pi Ltd. Modbus is a registered trademark of Schneider
Electric, licensed to the Modbus Organization, Inc. NaPi is the name of the
boards of the NaPi Lab team. Claude is a trademark of Anthropic, PBC. All other
names of products and companies are trademarks of their respective owners.
