# gonzocache

A login-time app launch tracker and page-cache preloader for Linux. A
companion project to [detritusd](https://github.com/TTR-IND/detritusd)
— a genuinely separate daemon, not a feature bolted onto it. See
[Why a separate daemon](#why-a-separate-daemon) below for the reasoning.

## What it does

Two modes, one binary:

- **`gonzocache --track`** runs continuously as a service, lightly
  polling `/proc` to detect new process launches, and keeps a
  persistent, exponentially-decayed launch score per executable in
  `/var/lib/gonzocache/history.json`. Frequent, recent launches score
  highest; a one-off launch from months ago decays toward
  irrelevance. This is the same weighting shape macOS's own
  launch-prediction cache uses — score by recency *and* frequency
  together, not either alone.

- **`gonzocache --preload`** runs once (meant to be triggered at
  login), reads the scored history, and calls
  `posix_fadvise(POSIX_FADV_WILLNEED)` on your most-used executables —
  pulling them into the page cache without executing them, so the
  apps you actually reach for tend to launch faster. It does not
  execute anything and does not run continuously.

## Why a separate daemon

detritusd's whole job is giving memory back — reactive freezing under
real pressure, proactive `MADV_COLD` trickling when idle. gonzocache's
job is the opposite: spending memory proactively on a bet that it
improves perceived launch speed. Mixing "give memory back" and
"consume memory speculatively" in one daemon would let the two
features fight each other's decisions in ways that are hard to reason
about. Two daemons with one clear job each stays honestly debuggable —
and each can be installed, run, and reasoned about independently of
the other.

gonzocache reads detritusd's live status (`/run/detritus/status.json`)
as a display data source in some contexts (see
[Gonzo System Monitor](https://github.com/TTR-IND/gonzo-system-monitor)),
but the two daemons make their own decisions independently — neither
depends on the other being installed or running.

## Status

**Developed and tested on Devuan Excalibur (OpenRC, no systemd), MATE
desktop, on real hardware.** The `/proc`-polling launch tracker was
specifically chosen after testing the alternative (Linux's proc
connector / netlink `PROC_EVENT_EXEC`) directly on this hardware and
finding it delivers zero genuine process-lifecycle events here — only
protocol-level acknowledgments. If your kernel/container setup
supports proc connector where this one didn't, `/proc`-polling would
still work correctly, just with slightly more overhead than an
event-driven approach would need.

It has **not** been tested on other distributions or init systems.
The core binary (`gonzocache.c`) has no OpenRC dependency itself —
only the provided service script does.

## Install

```bash
git clone https://github.com/TTR-IND/gonzocache.git
cd gonzocache
sudo ./install.sh
```

This builds `gonzocache`, installs an OpenRC service for continuous
`--track`, and installs a `.desktop` autostart entry so `--preload`
runs automatically at your next login. Since it's a login hook, you
won't see real preload activity until you actually log out and back
in (or reboot) — the tracker needs to observe some real launches
first before it has meaningful history to act on.

## Uninstall

```bash
sudo ./install.sh --uninstall
```

## Data

- `/var/lib/gonzocache/history.json` — persistent launch-score
  history, keyed by resolved executable path.
- `/var/lib/gonzocache/preloaded.list` — the list of files the most
  recent `--preload` run actually warmed, written for
  [detritusd](https://github.com/TTR-IND/detritusd) to compute
  real page-cache residency against (via `mincore()`) if you also have
  it installed. This is optional — gonzocache works standalone without
  detritusd present at all.

## License

Apache License 2.0. See `LICENSE`.
