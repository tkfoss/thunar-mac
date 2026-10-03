#!/bin/bash
# Package Thunar.app into a compressed disk image.
#
#   macos/make-dmg.sh [--version VERSION] [--background PNG] [--sign IDENTITY] Thunar.app [OUTPUT_DIR]
#
# Creates OUTPUT_DIR/Thunar-<version>-<arch>.dmg (default OUTPUT_DIR: the
# directory of Thunar.app) containing the bundle, an /Applications symlink
# and the license.
# VERSION defaults to CFBundleShortVersionString of the bundle.
#
# The image is built with "hdiutil create -srcfolder" as HFS+ in ULFO format
# (LZFSE, macOS 10.11+). hdiutil attaches a scratch image privately (not
# shown in Finder) while copying. "hdiutil makehybrid" would avoid that, but
# it stamps com.apple.FinderInfo on every file, which breaks
# "codesign --verify --strict" of the bundle inside the image.
# --background adds a .background/background.png (no Finder window layout is
# set). --sign signs the DMG with a Developer ID.
set -euo pipefail

version=""
background=""
identity=""
while [ $# -gt 0 ]; do
  case "$1" in
    --version) version="$2"; shift 2 ;;
    --background) background="$2"; shift 2 ;;
    --sign) identity="$2"; shift 2 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
app="${1:?usage: $0 [--version V] [--background PNG] [--sign ID] Thunar.app [OUTPUT_DIR]}"
app="$(cd "$app" && pwd)"
outdir="${2:-$(dirname "$app")}"
mkdir -p "$outdir"
outdir="$(cd "$outdir" && pwd)"

[ -x "$app/Contents/MacOS/Thunar" ] || { echo "$app is not a Thunar.app" >&2; exit 1; }
[ -n "$version" ] || version="$(plutil -extract CFBundleShortVersionString raw "$app/Contents/Info.plist")"
arch="$(lipo -archs "$app/Contents/MacOS/thunar-bin" | tr ' ' '-')"
dmg="$outdir/Thunar-$version-$arch.dmg"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
stage="$tmp/Thunar"
mkdir -p "$stage"
ditto "$app" "$stage/Thunar.app"
ln -s /Applications "$stage/Applications"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$repo/COPYING" ] && cp "$repo/COPYING" "$stage/License (GPL-2.0).txt"
if [ -n "$background" ]; then
  mkdir -p "$stage/.background"
  cp "$background" "$stage/.background/background.png"
fi

xattr -cr "$stage/Thunar.app"
rm -f "$dmg"
hdiutil create -quiet -volname "Thunar" -fs HFS+ -srcfolder "$stage" -format ULFO -ov "$dmg"

if [ -n "$identity" ]; then
  codesign --force --sign "$identity" --timestamp "$dmg"
fi

hdiutil verify -quiet "$dmg"
echo "created $dmg ($(du -h "$dmg" | cut -f1))"
