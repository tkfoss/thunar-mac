# Environment for building and running the Thunar macOS port from a prefix.
#
#   source macos/deps/env.sh [PREFIX]
#
# PREFIX defaults to $XFCE_PREFIX, else ~/thunar-mac-prefix. Only Homebrew and
# the system are put on PATH/pkg-config: other package managers (MacPorts in
# /opt/local, Nix, ...) must not leak into the build.

if [ -n "${1:-}" ]; then
  XFCE_PREFIX="$1"
fi
export XFCE_PREFIX="${XFCE_PREFIX:-$HOME/thunar-mac-prefix}"

if [ -z "${HOMEBREW_PREFIX:-}" ]; then
  if [ -x /opt/homebrew/bin/brew ]; then
    HOMEBREW_PREFIX=/opt/homebrew
  elif [ -x /usr/local/bin/brew ]; then
    HOMEBREW_PREFIX=/usr/local
  else
    HOMEBREW_PREFIX=/opt/homebrew
  fi
fi
export HOMEBREW_PREFIX
_hb="$HOMEBREW_PREFIX"

export PATH="$XFCE_PREFIX/bin:$_hb/opt/gnu-sed/libexec/gnubin:$_hb/opt/gettext/bin:$_hb/opt/libxslt/bin:$_hb/bin:$_hb/sbin:/usr/bin:/bin:/usr/sbin:/sbin"
export PKG_CONFIG_PATH="$XFCE_PREFIX/lib/pkgconfig:$XFCE_PREFIX/share/pkgconfig:$_hb/opt/libffi/lib/pkgconfig:$_hb/opt/libxml2/lib/pkgconfig:$_hb/opt/libxslt/lib/pkgconfig"
unset PKG_CONFIG_LIBDIR  # Homebrew pkgconf defaults (incl. the macOS SDK .pc files)
export ACLOCAL_PATH="$XFCE_PREFIX/share/aclocal:$_hb/share/aclocal"
export XDG_DATA_DIRS="$XFCE_PREFIX/share:$_hb/share"
export GSETTINGS_SCHEMA_DIR="$XFCE_PREFIX/share/glib-2.0/schemas"
export DYLD_FALLBACK_LIBRARY_PATH="$XFCE_PREFIX/lib:$_hb/lib"
export GIO_MODULE_DIR="$_hb/lib/gio/modules"
export XML_CATALOG_FILES="$_hb/etc/xml/catalog"
unset _hb
