# Jitter RNG Daemon

`jitterentropy-rngd` feeds the output of the Jitter RNG into the kernel's
`/dev/random`. `jitterentropy-rngd.c` is the daemon on every system; the
second source is how that system's kernel takes the data, and the directory of
its name holds its service files.

* **Linux** (`jitterentropy-rngd-linux.c`, `linux/`): the `RNDADDENTROPY`
  ioctl, which credits the entropy - once at startup, whenever the kernel's
  entropy estimate runs low, and every ten minutes regardless (by default;
  `--phase1` and `--phase2` change that schedule). It needs root
  (`CAP_SYS_ADMIN`).
* **FreeBSD, NetBSD, OpenBSD, macOS** (`jitterentropy-rngd-bsd.c`,
  `freebsd/`, `netbsd/`, `openbsd/`, `macos/`): a write to `/dev/random`, once
  at startup and every ten minutes after (`--interval`). None of these kernels
  counts entropy, except NetBSD for writes by root. The first seed is written
  with the privileges the daemon started with - root, except on macOS - and
  then it switches to `nobody` (`--user`) and, on FreeBSD and OpenBSD, enters
  Capsicum or `pledge(2)`. On NetBSD it stays root: from NetBSD 10 on, the
  kernel refuses every write to `/dev/random` by anyone else.

The man page `doc/jitterentropy-rngd.1` lists its options.

It used to be a project of its own,
<https://github.com/smuellerDD/jitterentropy-rngd>, carrying a copy of the
library. Here it links the library of this tree and shares its version;
`CHANGES.md` in this directory is its history up to that point.

## Build

	cmake -S . -B build -DENABLE_RNGD=ON
	cmake --build build
	cmake --install build

or, on Linux, linking the library's archive into the daemon,

	make ENABLE_RNGD=1
	make install ENABLE_RNGD=1

Both install the daemon to `sbin` and the service file of the system;
`make install` installs its man page as well. The systemd unit
`jitterentropy.service` goes to `lib/systemd/system` below the prefix;
`-DJENT_SYSTEMD_UNITDIR=<dir>` (`JENT_SYSTEMD_UNITDIR=<dir>` with make) names
another directory. The rc.d script `jitterentropy_rngd` goes to `etc/rc.d`
below the prefix on FreeBSD and to `/etc/rc.d` on NetBSD and OpenBSD, the
launchd plist `jitterentropy-rngd.plist` to `/Library/LaunchDaemons`;
`-DJENT_RNGD_SERVICEDIR=<dir>` names another directory.

## Systemd unit

	systemctl enable --now jitterentropy

The unit starts the daemon before `sysinit.target`, so that services needing
random numbers find `/dev/random` seeded. It is of `Type=notify`: the daemon
reports readiness once the kernel took its first injection.

## rc.d and launchd

	sysrc jitterentropy_rngd_enable=YES && service jitterentropy_rngd start	# FreeBSD
	echo jitterentropy_rngd=YES >> /etc/rc.conf && service jitterentropy_rngd start	# NetBSD
	rcctl enable jitterentropy_rngd && rcctl start jitterentropy_rngd	# OpenBSD
	launchctl bootstrap system /Library/LaunchDaemons/jitterentropy-rngd.plist	# macOS

## Docker

From the root of the source tree:

	docker build -f rngd/linux/Dockerfile -t jitterentropy-rngd .
	docker run -d --name=rngd --restart=always \
	    --cap-add=SYS_ADMIN --cap-drop=ALL \
	    --network=none jitterentropy-rngd

or `docker compose -f rngd/linux/docker-compose.yaml up -d`.

Both restart the container whenever the daemon ends, which has to be
considered for a permanent health test failure of the Jitter RNG: the daemon
ends on one with exit status 2, and the restarted daemon injects from a
new instance whose health tests start afresh. Where FIPS or NTG.1 mode
demands that a Jitter RNG which reported such a failure is not used any
more - on a host in FIPS mode as well, whose mode the container shares - drop
the restart policy, as no Docker policy tells this exit from others, or have
a supervising monitor act on the exit status.

## Random bytes

	jitterentropy-rngd --generate 32 --hex
	jitterentropy-rngd --generate 4096 --output seed.bin

write bytes of the Jitter RNG to standard output or to a file, as hex digits
with `--hex`, and exit without seeding the kernel - neither root nor
`/dev/random` is needed, and the log goes to standard error.
