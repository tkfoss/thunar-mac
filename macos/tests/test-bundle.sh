#!/bin/bash
# Headless checks of a self-contained Thunar.app (no window is opened).
#
#   macos/tests/test-bundle.sh [--prefix PREFIX] Thunar.app
#
# 1. make-app-bundle.py --verify: only system/@rpath dependencies, no
#    absolute build paths, valid signature
# 2. Contents/MacOS/Thunar --version in an empty environment (env -i, PATH
#    without Homebrew), with DYLD_PRINT_LIBRARIES: nothing outside the
#    bundle and the system may be loaded
# 3. bundle-harness.c, installed as thunar-bin in a copy of the bundle (at a
#    path with a space), started through the real launcher with an isolated
#    bus, cache and config: gdk-pixbuf SVG loader, GSettings, translations,
#    thunarx plugins, xfconf (activates xfconfd), thumbnailer activation and
#    the loaded images. Also checks that xfconfd and the thumbnailer run
#    from the copy.
# PREFIX (headers for the harness) defaults to $XFCE_PREFIX.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
prefix="${XFCE_PREFIX:-}"
if [ "${1:-}" = "--prefix" ]; then prefix="$2"; shift 2; fi
app="${1:?usage: $0 [--prefix PREFIX] Thunar.app}"
app="$(cd "$app" && pwd)"
[ -n "$prefix" ] || { echo "no prefix (--prefix or XFCE_PREFIX)" >&2; exit 2; }

fail=0
step() { printf '\n== %s\n' "$*"; }

step "verify $app"
python3 "$here/../make-app-bundle.py" --verify "$app" || fail=1

step "--version without Homebrew in the environment"
out="$(env -i HOME="$HOME" PATH=/usr/bin:/bin DYLD_PRINT_LIBRARIES=1 "$app/Contents/MacOS/Thunar" --version 2>&1)" || fail=1
echo "$out" | grep -v '^dyld\[' | grep -iv 'xfconf\|critical\|^$' || true
grep -q '^thunar [0-9]' <<<"$out" || { echo "FAIL: no version output"; fail=1; }
real_app="$(cd "$app" && pwd -P)"
outside="$(echo "$out" | sed -n 's/^dyld\[[0-9]*\]: <[^>]*> //p' \
  | grep -v "^$real_app/\|^$app/\|^/usr/lib/\|^/System/" || true)"
if [ -n "$outside" ]; then
  echo "FAIL: loaded from outside the bundle:"; echo "$outside"; fail=1
else
  echo "ok: $(echo "$out" | grep -c '^dyld\[') images, all from the bundle or the system"
fi

step "harness through the launcher"
# shellcheck source=../deps/env.sh
source "$here/../deps/env.sh" "$prefix"
tmp="$(mktemp -d /tmp/thunar-bundle-test.XXXXXX)"
copy="$tmp/with space/Thunar.app"
mkdir -p "$(dirname "$copy")"
cp -R "$app" "$copy"
fw="$copy/Contents/Frameworks"
# shellcheck disable=SC2046
cc -o "$copy/Contents/MacOS/thunar-bin" "$here/bundle-harness.c" \
  $(pkg-config --cflags thunarx-3 libxfconf-0 gdk-pixbuf-2.0 gio-2.0) \
  "$fw/libthunarx-3.0.dylib" "$fw/libxfconf-0.3.dylib" "$fw/libgdk_pixbuf-2.0.0.dylib" \
  "$fw/libgio-2.0.0.dylib" "$fw/libgobject-2.0.0.dylib" "$fw/libglib-2.0.0.dylib" "$fw/libintl.8.dylib" \
  -Wl,-rpath,@executable_path/../Frameworks -Wno-deprecated-declarations
codesign --force --sign - "$copy/Contents/MacOS/thunar-bin" >/dev/null
codesign --force --sign - "$copy" >/dev/null

cleanup() {
  pkill -f "$tmp/cache/dbus-session.conf" 2>/dev/null || true
  pkill -f "$copy/Contents/MacOS/" 2>/dev/null || true
  rm -rf "$tmp"
}
trap cleanup EXIT

env -i HOME="$HOME" PATH=/usr/bin:/bin LANG=de_DE.UTF-8 THUNAR_MACOS_LOG=- \
  THUNAR_MACOS_CACHE_DIR="$tmp/cache" THUNAR_DBUS_ADDRESS_FILE="$tmp/cache/bus-address" \
  XDG_CONFIG_HOME="$tmp/config" XDG_CACHE_HOME="$tmp/cache-home" \
  "$copy/Contents/MacOS/Thunar" || fail=1

for helper in xfconfd thunar-macos-thumbnailer dbus-daemon; do
  if pgrep -f "$copy/Contents/MacOS/$helper" >/dev/null; then
    echo "ok    $helper runs from the bundle copy"
  else
    echo "FAIL  $helper is not running from the bundle copy"; fail=1
  fi
done

echo
if [ "$fail" = 0 ]; then echo "ALL BUNDLE TESTS PASSED"; else echo "BUNDLE TESTS FAILED"; fi
exit "$fail"
