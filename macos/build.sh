#!/bin/bash
# Build the Thunar macOS port and its Xfce dependencies with Homebrew.
#
#   macos/build.sh [--prefix DIR] [--src DIR] [--builddir DIR] [--deps-only] [--jobs N]
#
#   --prefix DIR    install prefix (default: $XFCE_PREFIX, else ~/thunar-mac-prefix)
#   --src DIR       where the Xfce dependencies are cloned (default: PREFIX/../src)
#   --builddir DIR  Thunar meson build directory (default: <repo>/build-mac)
#   --deps-only     only build and install the dependencies
#
# Requirements: Homebrew with the formulae from macos/deps/Brewfile
# (brew bundle --file=macos/deps/Brewfile). Nothing from MacPorts or other
# package managers is used.
#
# Idempotent: each dependency is cloned/checked out at the revision pinned in
# macos/deps/REVISIONS, patched, built and installed only when its revision,
# patch or build options changed (stamp files in PREFIX/share/thunar-macos-deps).
# Thunar itself is then (re)configured, built and installed.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps_dir="$repo/macos/deps"

prefix="${XFCE_PREFIX:-$HOME/thunar-mac-prefix}"
src=""
builddir="$repo/build-mac"
deps_only=0
jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) prefix="$2"; shift 2 ;;
    --src) src="$2"; shift 2 ;;
    --builddir) builddir="$2"; shift 2 ;;
    --deps-only) deps_only=1; shift ;;
    --jobs) jobs="$2"; shift 2 ;;
    -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "$prefix"
prefix="$(cd "$prefix" && pwd)"
src="${src:-$(dirname "$prefix")/src}"
mkdir -p "$src"
src="$(cd "$src" && pwd)"
case "$builddir" in /*) ;; *) builddir="$PWD/$builddir" ;; esac

# shellcheck source=deps/env.sh
source "$deps_dir/env.sh" "$prefix"
# room for the install name rewriting of make-app-bundle.py
export LDFLAGS="${LDFLAGS:-} -Wl,-headerpad_max_install_names"

log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }

# sanity checks
[ "$(uname -s)" = Darwin ] || die "this script is for macOS"
command -v brew >/dev/null || die "Homebrew not found"
for tool in meson ninja pkg-config autoreconf glib-compile-resources msgfmt xsltproc sed; do
  command -v "$tool" >/dev/null || die "$tool not found (brew bundle --file=$deps_dir/Brewfile)"
done
sed --version 2>/dev/null | grep GNU >/dev/null || die "GNU sed not found (brew install gnu-sed)"
case "$(command -v pkg-config)" in
  "$HOMEBREW_PREFIX"/*) ;;
  *) die "pkg-config is not Homebrew's: $(command -v pkg-config)" ;;
esac

stamp_dir="$prefix/share/thunar-macos-deps"
mkdir -p "$stamp_dir"

common_meson_opts=(--prefix="$prefix" --libdir=lib --buildtype=debugoptimized
                   -Dvisibility=false -Dgtk-doc=false -Dintrospection=false -Dvala=disabled)

meson_opts_for() {
  case "$1" in
    libxfce4util) echo "${common_meson_opts[*]}" ;;
    xfconf) echo "${common_meson_opts[*]} -Dtests=false" ;;
    libxfce4ui) echo "${common_meson_opts[*]} -Dx11=disabled -Dwayland=disabled -Dsession-management=disabled" \
                     "-Dstartup-notification=disabled -Dlibgtop=disabled -Depoxy=disabled -Dgudev=disabled" ;;
  esac
}

patch_for() {
  [ -f "$deps_dir/$1-macos.patch" ] && echo "$deps_dir/$1-macos.patch" || true
}

checkout() { # name rev url
  local name="$1" rev="$2" url="$3" dir="$src/$1" patch
  patch="$(patch_for "$name")"
  if [ ! -d "$dir/.git" ]; then
    git clone --quiet "$url" "$dir"
  fi
  if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" != "$rev" ] || [ -n "$patch" ]; then
    git -C "$dir" cat-file -e "$rev^{commit}" 2>/dev/null || git -C "$dir" fetch --quiet origin
  fi
  if [ "$(git -C "$dir" rev-parse HEAD)" = "$rev" ] && [ -n "$patch" ] \
     && git -C "$dir" apply --reverse --check "$patch" 2>/dev/null \
     && [ "$(git -C "$dir" diff | shasum | cut -d' ' -f1)" = "$(shasum < "$patch" | cut -d' ' -f1)" ]; then
    return  # already at rev with exactly the patch applied (keep mtimes)
  fi
  if [ "$(git -C "$dir" rev-parse HEAD)" != "$rev" ] || [ -n "$(git -C "$dir" status --porcelain --untracked-files=no)" ]; then
    git -C "$dir" checkout --quiet --force "$rev"
  fi
  if [ -n "$patch" ]; then
    git -C "$dir" apply "$patch"
  fi
}

build_dep() { # name rev buildsys url
  local name="$1" rev="$2" buildsys="$3" url="$4" dir="$src/$1" key patch stamp
  patch="$(patch_for "$name")"
  key="$rev $buildsys $(meson_opts_for "$name") $LDFLAGS"
  if [ -n "$patch" ]; then
    key="$key $(shasum < "$patch" | cut -d' ' -f1)"
  fi
  stamp="$stamp_dir/$name.stamp"
  if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$key" ]; then
    log "$name: up to date ($rev)"
    return
  fi

  log "$name: building ${rev:0:7}"
  checkout "$name" "$rev" "$url"
  case "$buildsys" in
    autotools)
      (cd "$dir" && ./autogen.sh --prefix="$prefix" && make -j"$jobs" && make install)
      ;;
    meson)
      # fresh build directory: the options/LDFLAGS may have changed
      rm -rf "$dir/build"
      # shellcheck disable=SC2046
      meson setup "$dir/build" "$dir" $(meson_opts_for "$name")
      meson compile -C "$dir/build" -j "$jobs"
      meson install -C "$dir/build"
      ;;
    *) die "unknown build system $buildsys for $name" ;;
  esac
  echo "$key" > "$stamp"
}

log "prefix: $prefix"
log "deps sources: $src"
while read -r name rev buildsys url; do
  case "$name" in ''|'#'*) continue ;; esac
  build_dep "$name" "$rev" "$buildsys" "$url"
done < "$deps_dir/REVISIONS"

[ "$deps_only" = 1 ] && exit 0

thunar_opts=(--prefix="$prefix" --libdir=lib --buildtype=debugoptimized
             -Dvisibility=false -Dx11=disabled -Dsession-management=disabled -Dgudev=disabled
             -Dpolkit=disabled -Dterminal=disabled -Dlibnotify=disabled -Dlibcanberra=disabled
             -Dintrospection=false -Dthunar-tpa=disabled -Dgexiv2=disabled -Dthunar-wallpaper=disabled
             -Dthunarx-dirs-envvar=true)

log "Thunar: configuring $builddir"
if [ -f "$builddir/build.ninja" ]; then
  configured_prefix="$(meson introspect --buildoptions "$builddir" \
    | python3 -c 'import json,sys; print(next(o["value"] for o in json.load(sys.stdin) if o["name"]=="prefix"))')"
  [ "$configured_prefix" = "$prefix" ] \
    || die "$builddir is configured for prefix $configured_prefix, not $prefix (use --builddir)"
  meson setup --reconfigure "$builddir" "$repo" "${thunar_opts[@]}"
else
  meson setup "$builddir" "$repo" "${thunar_opts[@]}"
fi

log "Thunar: building"
meson compile -C "$builddir" -j "$jobs"
meson install -C "$builddir"
# plugins used to be built as .dylib, which GModule doesn't load
rm -f "$prefix"/lib/thunarx-3/*.dylib

log "done: $prefix/bin/thunar"
echo "Run it with:  $deps_dir/with-dbus.sh --prefix $prefix thunar"
echo "App bundle:   $repo/macos/make-app-bundle.sh --prefix $prefix $repo/build-mac/Thunar.app"
