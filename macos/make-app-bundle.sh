#!/bin/bash
# Build a self-contained, relocatable Thunar.app from an installed Thunar.
#
#   make-app-bundle.sh [--prefix PREFIX] [--sign IDENTITY] [OUTPUT/Thunar.app]
#   make-app-bundle.sh --verify Thunar.app
#
# See make-app-bundle.py for the bundle layout. The bundle needs neither
# Homebrew nor PREFIX at runtime. It is ad-hoc signed unless --sign is given,
# so Full Disk Access has to be granted again after every rebuild.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$here/make-app-bundle.py" "$@"
