# MSCPHUB

MSCP disk server for OpenVMS nodes, running on Linux.

MSCPHUB lets OpenVMS nodes on the same LAN use Linux files and block devices as
their own disks. To OpenVMS it looks like an HSC-style disk controller reached over
NISCA (the PEDRIVER protocol, ethertype 60-07). It is not a VMScluster member and
has no votes.

- User's guide: [doc/mscphub_guide.md](doc/mscphub_guide.md) (EN),
  [doc/mscphub_guide_ru.md](doc/mscphub_guide_ru.md) (RU) - install, configure,
  run as a service, troubleshoot, step by step.
- Manual page: `man 8 mscphub`. Help library: `mscphub /HELP`; sources in
  `doc/mscphublib.md` (EN) and `doc/mscphublib_ru.md` (RU).
- Version: X01-11ECO1.

What it does: up to 8 disks per server (files or block devices, 512 or 4096-octet
sectors, read-only units such as a CD image), READ, WRITE, ERASE and COMPARE over
the block transfers of NISCA, io_uring disk I/O with `fdatasync` before a WRITE is
answered, a systemd unit, its counters on SIGUSR1.

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
without `/etc/opt/mscphub/mscphub.conf`), a sample `mscphub.conf.example`, the
manual page, the help libraries and the guides. `cmake --build build --target
uninstall` removes what was installed; `--target dist` makes the kit of the
committed sources.

## Run

Check the configuration first: `/PASSIVE` only listens and sends nothing.

```
mscphub /CONFIG=/etc/opt/mscphub/mscphub.conf /PASSIVE
```

Then run it as a service: see the guide.

## Author

StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan)

## License

Copyright (C) 2026 StarLet Squad and Ruslan R. Laishev (AKA: BadAss SysMan).

MSCPHUB is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. It comes with ABSOLUTELY NO WARRANTY. See [LICENSE](LICENSE).

MSCPHUB is an independent implementation of the protocols, made for
interoperability. It contains no code of OpenVMS and is not affiliated with,
endorsed or supported by VMS Software, Inc. or Hewlett Packard Enterprise.
OpenVMS and VMScluster are trademarks of their owners; they are named here only
to say what MSCPHUB works with.
