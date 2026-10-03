#!/bin/bash
# Run a command in the Thunar macOS environment with a private session bus.
#
#   macos/deps/with-dbus.sh [--prefix DIR] <command> [args]
#
# The bus is started once and reused: its address is kept in
#   ~/Library/Caches/org.xfce.thunar/dbus-session-address
# (override with THUNAR_DBUS_ADDRESS_FILE), the same file Thunar.app uses, so
# both talk to the same Thunar and xfconfd. The bus configuration is the one
# installed by Thunar in $PREFIX/share/Thunar/macos/dbus-session.conf; its
# service directory is $PREFIX/share/dbus-1/services (xfconfd, thumbnailer).
# A bus another package manager put into launchd is deliberately ignored.
set -e

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ "${1:-}" = "--prefix" ]; then
  XFCE_PREFIX="$2"
  shift 2
fi
# shellcheck source=env.sh
source "$here/env.sh" "${XFCE_PREFIX:-}"

[ $# -gt 0 ] || { echo "usage: $0 [--prefix DIR] <command> [args]" >&2; exit 2; }

conf="$XFCE_PREFIX/share/Thunar/macos/dbus-session.conf"
[ -f "$conf" ] || { echo "missing $conf (build and install Thunar first)" >&2; exit 1; }

busfile="${THUNAR_DBUS_ADDRESS_FILE:-$HOME/Library/Caches/org.xfce.thunar/dbus-session-address}"
mkdir -p "$(dirname "$busfile")"

unset DBUS_SESSION_BUS_ADDRESS
if [ -s "$busfile" ]; then
  addr="$(cat "$busfile")"
  if "$HOMEBREW_PREFIX/bin/dbus-send" --bus="$addr" --dest=org.freedesktop.DBus --print-reply / \
       org.freedesktop.DBus.GetId >/dev/null 2>&1; then
    export DBUS_SESSION_BUS_ADDRESS="$addr"
  fi
fi
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
  "$HOMEBREW_PREFIX/bin/dbus-daemon" --config-file="$conf" --fork --print-address=1 > "$busfile"
  DBUS_SESSION_BUS_ADDRESS="$(head -n 1 "$busfile")"
  export DBUS_SESSION_BUS_ADDRESS
fi

exec "$@"
