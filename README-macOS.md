# Thunar for macOS

This is a native macOS port of [Thunar](https://docs.xfce.org/xfce/thunar/start), the Xfce file
manager. It uses GTK3's Quartz backend: no X11, no XQuartz, and no rewrite. The goal is to keep
Thunar's UI and features and make it usable as a Finder replacement on macOS.

It is an unofficial port and not supported by the Xfce project. Please report problems with the port
here, not upstream.

## Status

It works on macOS 15 (Apple Silicon) for daily use. What's implemented:

- Browsing, copying, moving, renaming, deleting, tabs, split view, search, bulk rename, custom
  actions, and the Thunar preferences, stored by xfconf.
- **Trash:** the macOS Trash (`~/.Trash` and the per-volume `.Trashes`) as `trash://`, including
  restore and Finder's "Put Back" locations.
  - Listing `~/.Trash` requires Full Disk Access.
- **Thumbnails:** Quick Look, through a D-Bus thumbnailer (`thunar-macos-thumbnailer`).
- **File types:** macOS content types (UTIs) mapped to MIME types where Thunar expects them.
- **Open With:** applications from LaunchServices plus custom commands; default applications can be
  set.
- **Send To:** `.desktop` targets. "Mail Recipient" composes a message in Mail.
- **Clipboard and drag and drop:** files are copied and cut through the macOS pasteboard, in
  Finder's format, so ⌘C/⌘X/⌘V work between Thunar windows, tabs and Finder. Drags carry all
  selected files. With no modifier key, a drop moves the files on the same disk and copies them
  otherwise. ⌥ forces a copy and ⌘ forces a move.
- **Volumes:** mounted volumes under `/Volumes` (USB disks, DMGs, network shares) in the side pane,
  with Eject.
- **Shortcuts:** macOS keyboard shortcuts (⌘C/⌘V, ⌘↑, ⌘[ ⌘], ⌘⌫, ⇧⌘., ⌘I, …).
- **Thunar.app:** a self-contained, relocatable app bundle with its own private D-Bus session bus.
  It's distributed as a DMG. Folders dropped on its Dock icon, or opened with "Open With → Thunar"
  in Finder, open in Thunar. Finder stays the default folder handler.

Known issues are listed [below](#known-issues).

## Platform

- **Apple Silicon (arm64) only.** The DMG is built on GitHub's `macos-15` runner.
- **Minimum macOS** is the version of the build machine, currently **macOS 15**. Homebrew bottles
  are built for the macOS version they run on, so the bundled libraries require it.
  `LSMinimumSystemVersion` in `Info.plist` is set to the highest `minos` of the bundled binaries.
- **Future work:**
  - An Intel build on a `macos-15-intel` runner (x86_64 Homebrew in `/usr/local`).
  - A universal build, either by merging the arm64 and x86_64 bundles with `lipo`, or by building
    the dependencies from source with `-arch arm64 -arch x86_64`.
  - Older macOS versions would need dependencies built from source with a lower
    `MACOSX_DEPLOYMENT_TARGET`, instead of Homebrew bottles.

## Installing

1. Download `Thunar-<version>-arm64.dmg`, either from a release or as a CI artifact.
2. Open the DMG and drag `Thunar.app` to `Applications`.
3. To see `~/.Trash`, grant Thunar Full Disk Access in System Settings > Privacy & Security >
   Full Disk Access.

Unless a release says it is notarized, the app is only ad-hoc signed, and Gatekeeper blocks the first
launch. Do one of the following:

- Open Thunar once and dismiss the warning. Then go to System Settings > Privacy & Security and click
  "Open Anyway" next to the Thunar message. (Since macOS 15, right-click → Open no longer bypasses
  Gatekeeper.)
- Or remove the quarantine attribute: `xattr -dr com.apple.quarantine /Applications/Thunar.app`.

Thunar.app needs neither Homebrew nor a build of Thunar. Its settings are stored in
`~/.config/xfce4/xfconf` and `~/.config/Thunar`, the same places the command-line build uses.

## Building

You need macOS on Apple Silicon, the Xcode Command Line Tools and [Homebrew](https://brew.sh). Nothing
from MacPorts or other package managers is used, and having them installed doesn't matter: the build
environment only contains Homebrew and the system.

```sh
git clone <this repository> thunar-mac && cd thunar-mac
brew bundle --file=macos/deps/Brewfile
macos/build.sh --prefix ~/thunar-mac-prefix
```

`macos/build.sh` does the following:

1. Clones the Xfce libraries Thunar needs at the revisions pinned in
   [`macos/deps/REVISIONS`](macos/deps/REVISIONS) into `PREFIX/../src` (change this with `--src DIR`).
   - The libraries are xfce4-dev-tools, libxfce4util, xfconf and libxfce4ui.
2. Applies the macOS patches from `macos/deps/*.patch`.
3. Builds the libraries and installs them into the prefix.
4. Configures, builds and installs Thunar. The build directory is `build-mac` (change it with
   `--builddir DIR`).

The script can be re-run safely. Dependencies are rebuilt only when their pinned revision, patch or
build options change, and Thunar is rebuilt incrementally. Run `macos/build.sh --help` for all
options.

To work in the build environment by hand, source the environment file:

```sh
source macos/deps/env.sh ~/thunar-mac-prefix
ninja -C build-mac && meson install -C build-mac
```

## Running from the build prefix

Thunar and xfconf need a D-Bus session bus, which macOS doesn't provide. `macos/deps/with-dbus.sh`
starts a private bus the first time and reuses it afterwards. xfconfd and the thumbnailer are
started on demand.

```sh
macos/deps/with-dbus.sh --prefix ~/thunar-mac-prefix thunar ~/Downloads
macos/deps/with-dbus.sh --prefix ~/thunar-mac-prefix thunar -q      # quit the Thunar daemon
```

The bus address is stored in `~/Library/Caches/org.xfce.thunar/dbus-session-address`. Thunar.app
uses the same file, so the command line and the app share one bus, one Thunar instance and one
xfconfd. Whichever starts the bus decides where xfconfd and the thumbnailer are activated from. To
use only the app's copies, quit the Thunar daemon and run `pkill -f dbus-session.conf`.

## Building Thunar.app and the DMG

```sh
macos/make-app-bundle.sh --prefix ~/thunar-mac-prefix build-mac/Thunar.app
macos/tests/test-bundle.sh --prefix ~/thunar-mac-prefix build-mac/Thunar.app   # headless checks
macos/make-dmg.sh build-mac/Thunar.app            # -> build-mac/Thunar-<version>-arm64.dmg
```

`make-app-bundle.py` copies everything Thunar needs into the bundle:

- `Contents/MacOS`: the launcher `Thunar`, `thunar-bin`, xfconfd, dbus-daemon and the helpers.
- `Contents/Frameworks`: every non-system dylib.
- `Contents/Resources/lib`: thunarx plugins, gdk-pixbuf loaders and GTK input methods.
- `Contents/Resources/share`: icons (Adwaita and hicolor), GSettings schemas, translations and
  Thunar's data.
- `Contents/Resources/etc/xdg`: default custom actions.

It then does the following:

1. Rewrites every library reference to `@rpath`.
2. Signs the bundle inside-out.
3. Verifies that no Mach-O file references anything outside the bundle or the system.
   - Run `make-app-bundle.sh --verify Thunar.app` to repeat this check on its own.

At launch, `Contents/MacOS/Thunar` does the following:

1. Derives every path from the bundle's location.
2. Writes the files that need absolute paths into `~/Library/Caches/org.xfce.thunar/`: loader
   caches, the bus configuration and the D-Bus service files.
3. Starts or reuses the private bus.
4. Execs `thunar-bin`.

Output goes to `~/Library/Logs/Thunar.log`.

`bundled-libraries.txt` in `Contents/Resources` lists the Xfce revisions and Homebrew versions
inside the bundle.

An ad-hoc signed bundle gets a new code identity with every build, so you have to grant Full Disk
Access again after each rebuild.

### Testing on a Mac without Homebrew

`macos/tests/test-bundle.sh` runs the bundle with an empty environment and `DYLD_PRINT_LIBRARIES`.
It proves that nothing is loaded from `/opt/homebrew` or from the prefix. To also try the GUI where
Homebrew doesn't exist, use one of these:

- **A macOS VM.** For example [lume](https://github.com/trycua/cua/tree/main/libs/lume) or UTM with
  a fresh macOS 15. Copy the DMG into the VM, install it, and work through the
  [manual test checklist](#manual-test-checklist).
- **Another Mac without Homebrew.**

A second user account on the same Mac is weaker evidence: `/opt/homebrew` is still readable there.
It does catch anything that depends on your home directory, though.

### Manual test checklist

The headless tests can't cover the UI. Before a release, check these by hand:

1. **Install and launch:** install from the DMG and get past Gatekeeper. Thunar.app opens `$HOME`
   with icons and thumbnails, the Dock shows its name and icon, and the UI is translated when the
   system language is not English.
2. **Trash:** grant Full Disk Access. Thunar lists `~/.Trash`, and Restore and Empty Trash work.
3. **File operations:** copy, move, rename and delete. Drag and drop inside Thunar and with Finder
   in both directions. Tabs, split view, search, bulk rename and custom actions.
4. **Shortcuts:** ⌘C/⌘V, ⌘↑, ⌘[ ⌘], ⌘⌫, ⌥⌘⌫, ⇧⌘., ⌘I, ⌘-click multi-select and ⌘W in dialogs.
5. **Open With and Send To:** open files with other apps and change the default app. Send To →
   Mail Recipient opens a Mail draft.
6. **Volumes:** attach a disk image (`hdiutil create -size 20m -fs APFS -volname TestVol /tmp/t.dmg &&
   hdiutil attach /tmp/t.dmg`). It appears under Devices within 2 s, Eject works, and ejecting a busy
   volume shows an error.
7. **Dock and Open With → Thunar:** with Thunar.app not running, drop a folder on its Dock icon.
   Exactly one window opens, showing that folder. With Thunar running, a dropped folder opens a new
   window (or a tab, if "open new windows as tabs" is set). In Finder, a folder's Open With → Thunar
   works, and double-clicking a folder still opens it in Finder.

## Continuous integration and releases

[`.github/workflows/macos.yml`](.github/workflows/macos.yml) runs on `macos-15` (arm64). It runs on
manual dispatch, on pull requests, and on pushes to `main` that touch the source. Documentation-only
changes don't trigger a build. Each run does the following:

1. Installs the Brewfile.
2. Restores the cached Xfce dependency prefix. The cache key is the hash of `macos/deps/*` plus the
   Homebrew versions, and a cache miss rebuilds the dependencies in a few minutes.
3. Runs `macos/build.sh`.
4. Smoke-tests `thunar --version`.
5. Builds Thunar.app and runs the headless bundle tests.
6. Builds the DMG, then mounts and verifies it on the runner.
7. Uploads the DMG, `bundled-libraries.txt` and `brew-versions.txt` as an artifact, kept for 14
   days.

Pushing a tag `v*` also creates a GitHub Release, for example:

```sh
git tag v4.21.6-mac1 && git push origin v4.21.6-mac1
```

The release contains the DMG and a note with the GPL source information:

- a link to the tagged source;
- a link to `macos/deps/REVISIONS` and the patches for the bundled Xfce libraries;
- the Homebrew versions of GTK, GLib and the other bundled libraries.

## Signing and notarization

Without credentials, CI produces an ad-hoc signed DMG (see [Installing](#installing) for the
Gatekeeper workaround). When the secrets below exist, the workflow does the following:

1. Imports the certificate into a temporary keychain.
2. Signs every Mach-O file inside-out with the hardened runtime (`--options runtime --timestamp`),
   then signs the bundle.
3. Notarizes and staples `Thunar.app`.
4. Builds and signs the DMG.
5. Notarizes and staples the DMG.

### Entitlements

No entitlements are used, for these reasons:

- **Library validation:** all code is inside the bundle and signed with the same Team ID, so library
  validation passes. thunarx plugins are only loaded from the bundle (`THUNARX_DIRS`), so
  `disable-library-validation` isn't needed.
- **DYLD variables:** the launcher doesn't use `DYLD_*` variables; libraries are found through
  `@rpath`. So `allow-dyld-environment-variables` isn't needed.
- **JIT:** without `allow-jit`, GLib's GRegex falls back from PCRE2's JIT to the interpreter.

`make-app-bundle.sh --entitlements FILE` exists in case a future change needs entitlements.

### What you need

1. **Apple Developer Program** membership (paid). You need the Account Holder role to create a
   Developer ID certificate.
2. **A Developer ID Application certificate.** Create it in one of two ways:
   - In Xcode: Settings > Accounts > your team > Manage Certificates > + > "Developer ID
     Application".
   - On developer.apple.com > Certificates > +, with a certificate signing request from Keychain
     Access (Certificate Assistant > Request a Certificate From a Certificate Authority, saved to
     disk).

   Then export it from Keychain Access as a `.p12` file with a password. In My Certificates, select
   "Developer ID Application: NAME (TEAMID)", including its private key, and use File > Export.
3. **An App Store Connect API key** for `notarytool`:
   1. In App Store Connect, go to Users and Access > Integrations > App Store Connect API > Team Keys
      and generate a key with the "Developer" role.
   2. Download `AuthKey_<KEYID>.p8`. You can only download it once.
   3. Note the Key ID and the Issuer ID, which is shown above the key list.

### Repository secrets

| Secret | Value |
|---|---|
| `MACOS_CERT_P12` | `base64 -i DeveloperID.p12` |
| `MACOS_CERT_PASSWORD` | the `.p12` export password |
| `MACOS_SIGN_IDENTITY` | `Developer ID Application: NAME (TEAMID)`, as shown by `security find-identity -v -p codesigning` |
| `AC_API_KEY_ID` | the key ID |
| `AC_API_ISSUER_ID` | the issuer ID |
| `AC_API_KEY_P8` | the contents of `AuthKey_<KEYID>.p8` |

Set them with the GitHub CLI:

```sh
base64 -i DeveloperID.p12 | gh secret set MACOS_CERT_P12
gh secret set MACOS_CERT_PASSWORD
gh secret set MACOS_SIGN_IDENTITY --body "Developer ID Application: NAME (TEAMID)"
gh secret set AC_API_KEY_ID --body KEYID
gh secret set AC_API_ISSUER_ID --body ISSUER-UUID
gh secret set AC_API_KEY_P8 < AuthKey_KEYID.p8
```

Signing alone works with only the first three secrets. Notarization additionally needs the API key.

A Developer ID signature also gives the app a stable identity, so Full Disk Access survives updates.
To sign locally, run `make-app-bundle.sh --sign "Developer ID Application: …"`. Then notarize with
`AC_API_KEY_PATH=… macos/notarize.sh file.zip|file.dmg` and run `xcrun stapler staple`.

## Known issues

- There is no global macOS menu bar; Thunar's menu bar stays in the window.
- Only mounted volumes are shown. Unmounted partitions can't be mounted from Thunar.
- The Quick Look thumbnail failure cache never expires. To reset it, delete
  `~/.cache/thumbnails/fail/thunar-macos`.
- Features that depend on Linux, X11 or the Xfce desktop are disabled: the terminal and wallpaper
  integration, polkit, udev, session management and notifications.
- "Edit Launcher" for `.desktop` files needs `xfce-desktop-item-edit`, which isn't bundled.

## License

Thunar is GPL-2.0-or-later and libthunarx is LGPL-2.0-or-later. See [COPYING](COPYING) and
[COPYING.LIB](COPYING.LIB). The macOS port keeps these licenses.

The DMG bundles libraries from Homebrew and Xfce under their own licenses. Mostly these are LGPL
(GTK, GLib, Pango, gdk-pixbuf, librsvg) or MIT-style (cairo, pixman, HarfBuzz, FreeType). D-Bus is
GPL-2.0-or-later or AFL-2.1. `Contents/Resources/bundled-libraries.txt` lists the exact versions.
