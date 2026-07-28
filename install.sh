#!/usr/bin/env bash
#
# install.sh -- installs gonzocache, a login-time app launch tracker
# and page-cache preloader for Linux.
#
# Design: staged, fail-loud, matching detritusd's own installer. Every
# run writes a full log to /var/log/gonzocache-install-<timestamp>.log
# regardless of outcome.
#
# Usage:
#   sudo ./install.sh              (build + install + start the --track service)
#   sudo ./install.sh --uninstall  (remove everything)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GONZOCACHE_SRC="$SCRIPT_DIR/gonzocache.c"
GONZOCACHE_BIN="/usr/local/sbin/gonzocache"
GONZOCACHE_INITD="/etc/init.d/gonzocache"
GONZOCACHE_OPENRC_SRC="$SCRIPT_DIR/gonzocache.openrc"
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

require_openrc() {
    if [ ! -d /run/openrc ]; then
        die "OpenRC does not appear to be the running init system (no /run/openrc). This installer targets OpenRC specifically. See the README for guidance on other init systems."
    fi
    command -v rc-update  >/dev/null 2>&1 || die "rc-update not found -- is openrc installed?"
    command -v rc-service >/dev/null 2>&1 || die "rc-service not found -- is openrc installed?"
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

install_gonzocache() {
    log "building gonzocache..."
    [ -f "$GONZOCACHE_SRC" ] || die "gonzocache.c not found at $GONZOCACHE_SRC"

    local tmp_bin="/tmp/gonzocache-build-$$"
    gcc -O2 -Wall -Wextra -Wno-unused-parameter \
        -o "$tmp_bin" "$GONZOCACHE_SRC" -lm \
        || die "gonzocache build failed"

    log "installing gonzocache to $GONZOCACHE_BIN"
    install -m 0755 -o root -g root "$tmp_bin" "$GONZOCACHE_BIN"
    rm -f "$tmp_bin"
    [ -x "$GONZOCACHE_BIN" ] || die "gonzocache installed but not executable at $GONZOCACHE_BIN"

    log "installing OpenRC service for gonzocache --track..."
    [ -f "$GONZOCACHE_OPENRC_SRC" ] || die "gonzocache.openrc not found at $GONZOCACHE_OPENRC_SRC"
    install -m 0755 -o root -g root "$GONZOCACHE_OPENRC_SRC" "$GONZOCACHE_INITD"

    log "adding gonzocache to the default runlevel..."
    rc-update add gonzocache default || die "rc-update add failed"

    if rc-service gonzocache status >/dev/null 2>&1; then
        log "gonzocache already running -- restarting to pick up new build"
        rc-service gonzocache restart || die "rc-service restart failed"
    else
        rc-service gonzocache start || die "rc-service start failed"
    fi

    sleep 1
    if ! rc-service gonzocache status | grep -q started; then
        warn "gonzocache did not stay running -- check your syslog for gonzocache entries"
    else
        log "gonzocache tracker is running"
    fi

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
    log "GonzoCache preload will run automatically at your next desktop login"
    log "  (it runs once per login, not continuously -- see $GONZOCACHE_AUTOSTART_FILE)"
}

uninstall_all() {
    log "stopping and removing gonzocache..."
    if command -v rc-service >/dev/null 2>&1; then
        rc-service gonzocache stop 2>/dev/null || true
    fi
    if command -v rc-update >/dev/null 2>&1; then
        rc-update delete gonzocache default 2>/dev/null || true
    fi
    rm -f "$GONZOCACHE_INITD"
    rm -f "$GONZOCACHE_BIN"
    rm -f "$GONZOCACHE_AUTOSTART_FILE"
    rm -rf /var/lib/gonzocache
    log "uninstall complete"
}

usage() {
    cat << EOF
Usage: sudo $0 [--uninstall]

  (no args)     install gonzocache (--track service + --preload autostart)
  --uninstall   remove everything

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
            require_openrc
            install_gonzocache
            ;;
        *)
            usage
            die "unrecognized argument: $1"
            ;;
    esac
    log "done."
    log "  tracker status:  rc-service gonzocache status"
    log "  full install log: $INSTALL_LOG"
}

main() {
    touch "$INSTALL_LOG" 2>/dev/null || INSTALL_LOG="/tmp/gonzocache-install-$(date +%Y%m%d-%H%M%S).log"
    main_inner "$@" 2>&1 | tee -a "$INSTALL_LOG"
    exit "${PIPESTATUS[0]}"
}

main "$@"
