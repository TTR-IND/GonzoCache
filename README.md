# gonzocache

A login-time app launch tracker and watermarked file cache for Linux.
A companion to [detritusd](https://github.com/TTR-IND/detritusd) — a
separate daemon, not a feature bolted onto it.

## What it does

Two modes, one binary:

- **`gonzocache --track`** runs as a service. It polls `/proc` for new
  process launches and keeps an exponentially-decayed score per
  executable in `/var/lib/gonzocache/history.json`. On the same
  process it holds the top-ranked executables — and the files those
  processes actually mapped at last launch — in the page cache, pinned
  with `mlock` while RAM is comfortable. A monitor thread drops that
  hold when `MemAvailable` falls below 400 MiB, and warms the list
  again when RAM recovers above 800 MiB.

- **`gonzocache --preload`** runs once at login as the desktop user.
  Same ranked list, `posix_fadvise(WILLNEED)` plus a sequential touch,
  then it unmaps and exits. It does not pin. The pages stay in cache
  only as long as the kernel leaves them.

## What changed in this revision

The public gonzocache release only hinted files in (`fadvise`) and
exited. Streaming I/O (YouTube is enough) then evicted them, so the
next launch was cold again. The older RookOS `hotcache` kept a hold
and purged on a watermark, which is the right policy — but it copied
each file into a private anonymous mapping and `mlock`ed *that*.
`exec()` does not read that mapping. It reads the page cache. The
hold was a second heap that cost RAM and did not accelerate launches.

This tree keeps gonzocache's tracker, scoring, history file,
`preloaded.list` contract, and packaging. The hold is file-backed
`mmap(MAP_SHARED)` of the real files, touched in 1 MiB strides,
optionally locked. Purge is `munlock` + `MADV_DONTNEED` + `munmap`.
Budget is 15% of RAM capped at 384 MiB — not the old 45%. 45% locked
on a 4 GiB machine is a fight with detritusd.

Associated files come from `/proc/pid/maps` at launch time, not from
`ldd`. That is the working set the process actually used.

## Why a separate daemon

detritusd gives anonymous memory back. gonzocache spends file-cache
on a launch-speed bet. One process doing both is two policies
fighting over `MemAvailable`. They share only
`/var/lib/gonzocache/preloaded.list`, which detritusd `mincore()`s
for a residency number in its status file.

## Watermarks

| condition                         | action                         |
|-----------------------------------|--------------------------------|
| MemAvailable < 400 MiB            | purge hold until ≥ 600 MiB     |
| MemAvailable > 800 MiB and empty  | warm from history again        |
| file larger than 32 MiB           | only the first 32 MiB is held  |

Shells, the two daemons, and `/proc`/`/dev` paths are not tracked.

## Install

```bash
sudo ./install.sh
```

Builds the binary, installs a `--track` service for whatever is
PID 1 (OpenRC or runit), and installs a `.desktop` autostart so
`--preload` runs at login.

runit status and logs:

```bash
sudo sv status gonzocache
tail /var/log/gonzocache/current
```

## Uninstall

```bash
sudo ./install.sh --uninstall
```

## Data

- `/var/lib/gonzocache/history.json` — launch scores and associated
  file lists. Schema version stays 1; `files` is an additive array.
- `/var/lib/gonzocache/preloaded.list` — paths the most recent warm
  actually held. detritusd reads this if present.
- `/var/lib/gonzocache/stats` — `files_held`, `held_mb`, `budget_mb`.

## License

Apache License 2.0. See `LICENSE`.
