#!/usr/bin/env bash
#
# install.sh -- installs gonzocache.
#
# Init is detected from PID 1. OpenRC and runit are both first-class.
# --track is the service. --preload is a desktop autostart (no init).
#
# Usage:
#   sudo ./install.sh              (build + install + start --track)
#   sudo ./install.sh --uninstall  (remove everything)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GONZOCACHE_SRC="$SCRIPT_DIR/gonzocache.c"
GONZOCACHE_BIN="/usr/local/sbin/gonzocache"
GONZOCACHE_INITD="/etc/init.d/gonzocache"
GONZOCACHE_OPENRC_SRC="$SCRIPT_DIR/gonzocache.openrc"
GONZOCACHE_RUNIT_SRC="$SCRIPT_DIR/runit"
GONZOCACHE_SVDIR="/etc/sv/gonzocache"
GONZOCACHE_AUTOSTART_DIR="/etc/xdg/autostart"
GONZOCACHE_AUTOSTART_FILE="$GONZOCACHE_AUTOSTART_DIR/gonzocache-preload.desktop"

INSTALL_LOG="/var/log/gonzocache-install-$(date +%Y%m%d-%H%M%S).log"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()  { echo -e "${GREEN}[gonzocache-install]${NC} $*"; }
warn() { echo -e "${YELLOW}[gonzocache-install] WARNING:${NC} $*"; }
die()  { echo -e "${RED}[gonzocache-install] ERROR:${NC} $*" >&2; exit 1; }

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        die "must be run as root (sudo ./install.sh)"
    fi
}

detect_init() {
    local comm="" exe=""
    comm="$(ps -p 1 -o comm= 2>/dev/null | tr -d ' ' || true)"
    exe="$(readlink -f /proc/1/exe 2>/dev/null || true)"

    case "$comm" in
        runit|runit-init) echo runit; return ;;
        systemd)          echo systemd; return ;;
        openrc-init)      echo openrc; return ;;
    esac
    case "$exe" in
        *runit*)   echo runit; return ;;
        *systemd*) echo systemd; return ;;
        *openrc*)  echo openrc; return ;;
    esac
    if [ -d /run/openrc ] && command -v rc-service >/dev/null 2>&1; then
        echo openrc
        return
    fi
    if [ -d /etc/sv ] && command -v sv >/dev/null 2>&1; then
        echo runit
        return
    fi
    echo unknown
}

runit_supervise_dir() {
    if [ -d /etc/service ]; then
        echo /etc/service
    elif [ -d /var/service ]; then
        echo /var/service
    elif [ -d /etc/runit/runsvdir/default ]; then
        echo /etc/runit/runsvdir/default
    else
        echo ""
    fi
}

check_dependencies() {
    log "checking build dependencies..."
    local missing=()
    for pkg in build-essential gcc; do
        dpkg -s "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
    done
    if [ "${#missing[@]}" -gt 0 ]; then
        log "installing missing packages: ${missing[*]}"
        apt-get update || die "apt-get update failed"
        apt-get install -y "${missing[@]}" || die "apt-get install failed for: ${missing[*]}"
    else
        log "all build dependencies already present"
    fi
}

install_binary() {
    log "building gonzocache..."
    [ -f "$GONZOCACHE_SRC" ] || die "gonzocache.c not found at $GONZOCACHE_SRC"

    local tmp_bin="/tmp/gonzocache-build-$$"
    gcc -O2 -Wall -Wextra -Wno-unused-parameter \
        -o "$tmp_bin" "$GONZOCACHE_SRC" -lm -lpthread \
        || die "gonzocache build failed"

    log "installing gonzocache to $GONZOCACHE_BIN"
    install -m 0755 -o root -g root "$tmp_bin" "$GONZOCACHE_BIN"
    rm -f "$tmp_bin"
    [ -x "$GONZOCACHE_BIN" ] || die "gonzocache installed but not executable at $GONZOCACHE_BIN"
}

install_autostart() {
    log "installing login-time preload autostart entry..."
    mkdir -p "$GONZOCACHE_AUTOSTART_DIR"
    cat > "$GONZOCACHE_AUTOSTART_FILE" << EOF
[Desktop Entry]
Type=Application
Name=GonzoCache Preload
Comment=Warms commonly-used apps into page cache at login
Exec=$GONZOCACHE_BIN --preload
X-GNOME-Autostart-enabled=true
NoDisplay=true
EOF
    chmod 0644 "$GONZOCACHE_AUTOSTART_FILE"
    log "preload will run automatically at your next desktop login"
    log "  (once per login — $GONZOCACHE_AUTOSTART_FILE)"
}

install_openrc() {
    command -v rc-update  >/dev/null 2>&1 || die "rc-update not found -- OpenRC tools missing"
    command -v rc-service >/dev/null 2>&1 || die "rc-service not found -- OpenRC tools missing"
    [ -f "$GONZOCACHE_OPENRC_SRC" ] || die "gonzocache.openrc not found at $GONZOCACHE_OPENRC_SRC"

    log "installing OpenRC service for --track..."
    install -m 0755 -o root -g root "$GONZOCACHE_OPENRC_SRC" "$GONZOCACHE_INITD"
    rc-update add gonzocache default || die "rc-update add failed"

    if rc-service gonzocache status >/dev/null 2>&1; then
        log "gonzocache already running -- restarting"
        rc-service gonzocache restart || die "rc-service restart failed"
    else
        rc-service gonzocache start || die "rc-service start failed"
    fi

    sleep 1
    if ! rc-service gonzocache status | grep -q started; then
        warn "gonzocache did not stay running -- check syslog for gonzocache entries"
    else
        log "gonzocache --track is running (OpenRC)"
    fi
}

install_runit() {
    command -v sv >/dev/null 2>&1 || die "sv not found -- runit tools missing"
    [ -f "$GONZOCACHE_RUNIT_SRC/run" ] || die "runit/run not found at $GONZOCACHE_RUNIT_SRC/run"

    local supervise
    supervise="$(runit_supervise_dir)"
    [ -n "$supervise" ] || die "no runit supervise directory found (/etc/service, /var/service, or /etc/runit/runsvdir/default)"

    log "installing runit service to $GONZOCACHE_SVDIR..."
    mkdir -p "$GONZOCACHE_SVDIR/log"
    install -m 0755 -o root -g root "$GONZOCACHE_RUNIT_SRC/run" "$GONZOCACHE_SVDIR/run"
    install -m 0755 -o root -g root "$GONZOCACHE_RUNIT_SRC/log/run" "$GONZOCACHE_SVDIR/log/run"

    export SVDIR="$supervise"

    if [ -L "$supervise/gonzocache" ] || [ -d "$supervise/gonzocache" ]; then
        log "gonzocache already supervised -- restarting"
        sv restart gonzocache || true
        sv down gonzocache 2>/dev/null || true
        sv up gonzocache || die "sv up failed"
    else
        log "enabling gonzocache under $supervise..."
        ln -s "$GONZOCACHE_SVDIR" "$supervise/gonzocache"
    fi

    local i status=""
    for i in 1 2 3 4 5 6 7 8 9 10; do
        sleep 1
        status="$(sv status gonzocache 2>/dev/null || true)"
        case "$status" in
            run:*) break ;;
        esac
    done
    if [ -n "$status" ]; then
        log "sv status: $status"
    fi
    case "$status" in
        run:*)
            log "gonzocache --track is running (runit)"
            ;;
        *)
            warn "supervise has not reported run: yet"
            warn "  check: SVDIR=$supervise sv status gonzocache"
            warn "  logs:  tail /var/log/gonzocache/current"
            ;;
    esac
}

uninstall_all() {
    log "stopping and removing gonzocache..."
    if command -v sv >/dev/null 2>&1; then
        sv down gonzocache 2>/dev/null || true
        sv force-stop gonzocache 2>/dev/null || true
    fi
    if command -v rc-service >/dev/null 2>&1; then
        rc-service gonzocache stop 2>/dev/null || true
    fi
    if command -v rc-update >/dev/null 2>&1; then
        rc-update delete gonzocache default 2>/dev/null || true
    fi

    local supervise
    supervise="$(runit_supervise_dir)"
    if [ -n "$supervise" ]; then
        rm -f "$supervise/gonzocache"
    fi
    rm -rf "$GONZOCACHE_SVDIR"
    rm -f "$GONZOCACHE_INITD"
    rm -f "$GONZOCACHE_BIN"
    rm -f "$GONZOCACHE_AUTOSTART_FILE"
    rm -rf /var/lib/gonzocache
    rm -rf /var/log/gonzocache
    log "uninstall complete"
}

usage() {
    cat << EOF
Usage: sudo $0 [--uninstall]

  (no args)     install gonzocache (--track service + --preload autostart)
  --uninstall   remove everything

Init is detected from PID 1 (OpenRC or runit).
Every run writes a full log to /var/log/gonzocache-install-<timestamp>.log.
EOF
}

main_inner() {
    require_root
    case "${1:-}" in
        --uninstall)
            uninstall_all
            exit 0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        "")
            check_dependencies
            local init
            init="$(detect_init)"
            log "detected init: $init"
            install_binary
            case "$init" in
                runit)  install_runit ;;
                openrc) install_openrc ;;
                systemd)
                    die "PID 1 is systemd. No unit ships in this tree."
                    ;;
                *)
                    die "could not detect OpenRC or runit as PID 1"
                    ;;
            esac
            install_autostart
            ;;
        *)
            usage
            die "unrecognized argument: $1"
            ;;
    esac
    log "done."
    log "  full install log: $INSTALL_LOG"
}

main() {
    touch "$INSTALL_LOG" 2>/dev/null || INSTALL_LOG="/tmp/gonzocache-install-$(date +%Y%m%d-%H%M%S).log"
    main_inner "$@" 2>&1 | tee -a "$INSTALL_LOG"
    exit "${PIPESTATUS[0]}"
}

main "$@"
