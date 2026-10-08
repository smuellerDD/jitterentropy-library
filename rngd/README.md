# Jitter RNG Daemon

`jitterentropy-rngd` feeds the output of the Jitter RNG into the Linux
`/dev/random` device with the `RNDADDENTROPY` ioctl: once at startup, whenever
the kernel's entropy estimate runs low, and every ten minutes regardless (by
default; `--phase1` and `--phase2` change that schedule). It needs root
(`CAP_SYS_ADMIN`). The man page `doc/jitterentropy-rngd.1` lists its options.

It used to be a project of its own,
<https://github.com/smuellerDD/jitterentropy-rngd>, carrying a copy of the
library. Here it links the library of this tree and shares its version;
`CHANGES.md` in this directory is its history up to that point.

## Build

	cmake -S . -B build -DENABLE_RNGD=ON
	cmake --build build
	cmake --install build

or, linking the library's archive into the daemon,

	make ENABLE_RNGD=1
	make install ENABLE_RNGD=1

Both install the daemon to `sbin` and the systemd unit `jitterentropy.service`;
`make install` installs its man page as well. The unit goes to
`lib/systemd/system` below the prefix; `-DJENT_SYSTEMD_UNITDIR=<dir>`
(`JENT_SYSTEMD_UNITDIR=<dir>` with make) names another directory.

## Systemd unit

	systemctl enable --now jitterentropy

The unit starts the daemon before `sysinit.target`, so that services needing
random numbers find `/dev/random` seeded. It is of `Type=notify`: the daemon
reports readiness once the kernel took its first injection.

## Docker

From the root of the source tree:

	docker build -f rngd/Dockerfile -t jitterentropy-rngd .
	docker run -d --name=rngd --restart=always \
	    --cap-add=SYS_ADMIN --cap-drop=ALL \
	    --network=none jitterentropy-rngd

or `docker compose -f rngd/docker-compose.yaml up -d`.
