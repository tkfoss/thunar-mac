#!/usr/bin/env python3
"""Assemble a self-contained, relocatable Thunar.app from an installed Thunar.

    make-app-bundle.py [--prefix PREFIX] [--homebrew DIR] [--sign IDENTITY]
                       [--entitlements FILE] [OUTPUT/Thunar.app]
    make-app-bundle.py --verify Thunar.app

The bundle needs neither Homebrew nor the install prefix at runtime:

  Contents/MacOS/        Thunar (launcher, see thunar-app-launcher.c), thunar-bin,
                         thunar-sendto-email, thunar-macos-thumbnailer, xfconfd,
                         xfconf-query, dbus-daemon, dbus-send
  Contents/Frameworks/   every non-system dylib, install name @rpath/<name>
  Contents/Resources/    lib/thunarx-3 (plugins), lib/gdk-pixbuf-2.0 (loaders),
                         lib/gtk-3.0 (input methods), lib/gio/modules (empty),
                         share/{icons,locale,glib-2.0/schemas,themes,Thunar},
                         etc/xdg, runtime/ (templates the launcher instantiates
                         into ~/Library/Caches/org.xfce.thunar with absolute paths)

Every Mach-O file gets its dependencies rewritten to @rpath/<name> and a single
LC_RPATH pointing at Contents/Frameworks relative to itself, then it is signed:
ad-hoc by default, or with --sign IDENTITY (hardened runtime + timestamp).
"""

import argparse
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

SYSTEM_PREFIXES = ("/usr/lib/", "/System/")
MACHO_MAGICS = (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")

# translation domains bundled per language
PREFIX_DOMAINS = ("thunar", "libxfce4ui", "libxfce4util", "xfconf")
HOMEBREW_DOMAINS = ("gtk30", "glib20")


def log(msg):
    print(msg, flush=True)


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def run(*cmd, check=True):
    res = subprocess.run(cmd, capture_output=True, text=True)
    if check and res.returncode != 0:
        die(f"{' '.join(cmd)} failed:\n{res.stdout}{res.stderr}")
    return res.stdout


def is_macho(path):
    if os.path.islink(path) or not os.path.isfile(path):
        return False
    with open(path, "rb") as fp:
        return fp.read(4) in MACHO_MAGICS


def load_commands(path):
    """Return (id, [deps], [rpaths], minos) of a Mach-O file."""
    out = run("otool", "-l", path)
    ident, deps, rpaths, minos = None, [], [], None
    cmd = None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("cmd "):
            cmd = line.split()[1]
        elif line.startswith("name ") and cmd in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB",
                                                    "LC_LAZY_LOAD_DYLIB", "LC_LOAD_UPWARD_DYLIB"):
            deps.append(line[5:].rsplit(" (offset", 1)[0])
        elif line.startswith("name ") and cmd == "LC_ID_DYLIB":
            ident = line[5:].rsplit(" (offset", 1)[0]
        elif line.startswith("path ") and cmd == "LC_RPATH":
            rpaths.append(line[5:].rsplit(" (offset", 1)[0])
        elif line.startswith("minos ") and cmd == "LC_BUILD_VERSION":
            minos = line.split()[1]
    return ident, deps, rpaths, minos


def is_system(dep):
    return dep.startswith(SYSTEM_PREFIXES)


class Bundler:
    def __init__(self, args):
        self.prefix = os.path.realpath(args.prefix)
        self.homebrew = os.path.realpath(args.homebrew)
        self.app = os.path.abspath(args.output)
        self.identity = args.sign
        self.entitlements = args.entitlements
        self.contents = os.path.join(self.app, "Contents")
        self.macos = os.path.join(self.contents, "MacOS")
        self.frameworks = os.path.join(self.contents, "Frameworks")
        self.resources = os.path.join(self.contents, "Resources")
        self.runtime = os.path.join(self.resources, "runtime")
        # bundled Mach-O file -> source path (for resolving @loader_path/@rpath)
        self.sources = {}
        # real source path of a dylib -> name in Frameworks
        self.framework_names = {}

    # -- helpers -------------------------------------------------------------

    def hb(self, *parts):
        return os.path.join(self.homebrew, *parts)

    def pf(self, *parts):
        return os.path.join(self.prefix, *parts)

    def copy_macho(self, src, dst):
        src = os.path.realpath(src)
        if not os.path.isfile(src):
            die(f"missing {src}")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        os.chmod(dst, 0o755)
        self.sources[dst] = src

    def copytree(self, src, dst, ignore=None):
        shutil.copytree(os.path.realpath(src), dst, symlinks=True, ignore=ignore, dirs_exist_ok=True)

    # -- Mach-O files --------------------------------------------------------

    def collect_binaries(self):
        log("copying executables, plugins and modules")
        exes = {
            "thunar-bin": self.pf("bin", "thunar"),
            "thunar-sendto-email": self.pf("lib", "Thunar", "thunar-sendto-email"),
            "thunar-macos-thumbnailer": self.pf("lib", "Thunar", "thunar-macos-thumbnailer"),
            "xfconfd": self.pf("lib", "xfce4", "xfconf", "xfconfd"),
            "xfconf-query": self.pf("bin", "xfconf-query"),
            "dbus-daemon": self.hb("bin", "dbus-daemon"),
            "dbus-send": self.hb("bin", "dbus-send"),
        }
        for name, src in exes.items():
            self.copy_macho(src, os.path.join(self.macos, name))
        self.copy_macho(self.pf("share", "Thunar", "macos", "thunar-app-launcher"), os.path.join(self.macos, "Thunar"))

        # thunarx plugins (the wallpaper plugin needs an Xfce/X11 desktop)
        plugdir = self.pf("lib", "thunarx-3")
        for f in sorted(os.listdir(plugdir)):
            if f.endswith(".so") and "wallpaper" not in f:  # G_MODULE_SUFFIX
                self.copy_macho(os.path.join(plugdir, f), os.path.join(self.resources, "lib", "thunarx-3", f))

        # gdk-pixbuf loaders (PNG/JPEG are built in; SVG comes from librsvg)
        self.copy_module_dir(self.hb("lib", "gdk-pixbuf-2.0", "2.10.0"), "loaders",
                             os.path.join("lib", "gdk-pixbuf-2.0", "2.10.0"), "loaders.cache")
        # GTK input methods (im-quartz: dead keys and input sources)
        self.copy_module_dir(self.hb("lib", "gtk-3.0", "3.0.0"), "immodules",
                             os.path.join("lib", "gtk-3.0", "3.0.0"), "immodules.cache")
        # no GIO modules, but GIO_MODULE_DIR must not fall back to Homebrew's
        os.makedirs(os.path.join(self.resources, "lib", "gio", "modules"), exist_ok=True)

    def copy_module_dir(self, base, subdir, dest_rel, cache_name):
        srcdir = os.path.join(base, subdir)
        dstdir = os.path.join(self.resources, dest_rel, subdir)
        for f in sorted(os.listdir(srcdir)):
            if f.endswith(".so"):
                self.copy_macho(os.path.join(srcdir, f), os.path.join(dstdir, f))
        # the cache lists modules by absolute path: make it a template
        template = []
        with open(os.path.join(base, cache_name)) as fp:
            for line in fp:
                m = re.match(r'^"(/[^"]*/)([^/"]+\.so)"(.*)$', line.rstrip("\n"))
                if m:
                    line = f'"@RESOURCES@/{dest_rel}/{subdir}/{m.group(2)}"{m.group(3)}\n'
                    if not os.path.exists(os.path.join(dstdir, m.group(2))):
                        die(f"{cache_name} lists {m.group(2)}, which is not in {srcdir}")
                elif re.search(r'"/[^"]*/share/locale"', line):
                    line = re.sub(r'"/[^"]*/share/locale"', '"@RESOURCES@/share/locale"', line)
                elif line.startswith("#") and ("/opt/" in line or "/Users/" in line or "/usr/local" in line):
                    continue  # comments with build paths
                template.append(line)
        os.makedirs(self.runtime, exist_ok=True)
        with open(os.path.join(self.runtime, cache_name), "w") as fp:
            fp.writelines(template)

    def resolve(self, dep, referrer_src, rpaths):
        """Find the file a dependency of referrer_src refers to."""
        loader_dir = os.path.dirname(referrer_src)
        candidates = []
        if dep.startswith("@rpath/"):
            rest = dep[len("@rpath/"):]
            for rp in rpaths:
                rp = rp.replace("@loader_path", loader_dir).replace("@executable_path", loader_dir)
                candidates.append(os.path.join(rp, rest))
            candidates += [self.pf("lib", rest), self.hb("lib", rest)]
        elif dep.startswith("@loader_path/") or dep.startswith("@executable_path/"):
            candidates.append(os.path.join(loader_dir, dep.split("/", 1)[1]))
        else:
            candidates.append(dep)
        for c in candidates:
            if os.path.exists(c):
                return os.path.realpath(c)
        die(f"cannot resolve {dep} (needed by {referrer_src})")

    def collect_frameworks(self):
        log("collecting dylibs")
        os.makedirs(self.frameworks, exist_ok=True)
        queue = list(self.sources.items())
        self.deps = {}  # bundled file -> [(dep as written, framework name)]
        while queue:
            dst, src = queue.pop()
            _, deps, rpaths, _ = load_commands(src)
            resolved = []
            for dep in deps:
                if is_system(dep):
                    continue
                real = self.resolve(dep, src, rpaths)
                name = self.framework_names.get(real)
                if name is None:
                    name = os.path.basename(dep)
                    if name in self.framework_names.values():
                        name = os.path.basename(real)
                        if name in self.framework_names.values():
                            die(f"two different libraries named {name}")
                    self.framework_names[real] = name
                    fdst = os.path.join(self.frameworks, name)
                    self.copy_macho(real, fdst)
                    queue.append((fdst, real))
                resolved.append((dep, name))
            self.deps[dst] = resolved
        log(f"  {len(self.framework_names)} dylibs")

    def fix_install_names(self):
        log("rewriting install names")
        for dst in sorted(self.deps):
            ident, _, rpaths, _ = load_commands(dst)
            in_frameworks = os.path.dirname(dst) == self.frameworks
            if in_frameworks:
                wanted = "@loader_path"
            else:
                rel = os.path.relpath(self.frameworks, os.path.dirname(dst))
                base = "@executable_path" if os.path.dirname(dst) == self.macos else "@loader_path"
                wanted = f"{base}/{rel}"
            cmd = ["install_name_tool"]
            if ident is not None:
                cmd += ["-id", f"@rpath/{os.path.basename(dst)}" if in_frameworks else os.path.basename(dst)]
            for dep, name in self.deps[dst]:
                if dep != f"@rpath/{name}":
                    cmd += ["-change", dep, f"@rpath/{name}"]
            for rp in dict.fromkeys(rpaths):
                if rp != wanted:
                    cmd += ["-delete_rpath", rp]
            if self.deps[dst] and wanted not in rpaths:
                cmd += ["-add_rpath", wanted]
            if len(cmd) > 1:
                os.chmod(dst, 0o755)
                res = subprocess.run(cmd + [dst], capture_output=True, text=True)
                errors = [l for l in res.stderr.splitlines() if "will invalidate the code signature" not in l]
                if res.returncode != 0 or errors:
                    die(f"install_name_tool failed for {dst}:\n{res.stderr}")

    # -- data ----------------------------------------------------------------

    def copy_data(self):
        log("copying data")
        share = os.path.join(self.resources, "share")

        # Info.plist, PkgInfo, icon
        shutil.copyfile(self.pf("share", "Thunar", "macos", "Info.plist"), os.path.join(self.contents, "Info.plist"))
        with open(os.path.join(self.contents, "PkgInfo"), "w") as fp:
            fp.write("APPL????")
        self.make_icon()

        # icons: Adwaita without cursors (GDK Quartz uses NSCursor), hicolor + Thunar's
        adwaita = os.path.dirname(os.path.realpath(self.hb("share", "icons", "Adwaita", "index.theme")))
        self.copytree(adwaita, os.path.join(share, "icons", "Adwaita"), ignore=shutil.ignore_patterns("cursors"))
        hicolor = os.path.dirname(os.path.realpath(self.hb("share", "icons", "hicolor", "index.theme")))
        os.makedirs(os.path.join(share, "icons", "hicolor"), exist_ok=True)
        shutil.copyfile(os.path.join(hicolor, "index.theme"), os.path.join(share, "icons", "hicolor", "index.theme"))
        self.copytree(self.pf("share", "icons", "hicolor"), os.path.join(share, "icons", "hicolor"))
        for theme in ("Adwaita", "hicolor"):
            path = os.path.join(share, "icons", theme)
            cache = os.path.join(path, "icon-theme.cache")
            if os.path.exists(cache):
                os.remove(cache)
            run(self.hb("bin", "gtk3-update-icon-cache"), "-q", "-f", "-t", path)

        # GSettings schemas of GTK (file chooser, color chooser, ...)
        schemas = os.path.join(share, "glib-2.0", "schemas")
        os.makedirs(schemas, exist_ok=True)
        gtk_schemas = os.path.join(os.path.realpath(self.hb("opt", "gtk+3")), "share", "glib-2.0", "schemas")
        for f in os.listdir(gtk_schemas):
            if f.startswith("org.gtk.Settings.") and f.endswith(".gschema.xml"):
                shutil.copyfile(os.path.join(gtk_schemas, f), os.path.join(schemas, f))
        prefix_schemas = self.pf("share", "glib-2.0", "schemas")
        if os.path.isdir(prefix_schemas):
            for f in os.listdir(prefix_schemas):
                if f.endswith(".xml"):
                    shutil.copyfile(os.path.join(prefix_schemas, f), os.path.join(schemas, f))
        run(self.hb("bin", "glib-compile-schemas"), schemas)

        # GTK key themes ("Mac" has the macOS text editing bindings)
        gtk_themes = os.path.join(os.path.realpath(self.hb("opt", "gtk+3")), "share", "themes")
        if os.path.isdir(gtk_themes):
            self.copytree(gtk_themes, os.path.join(share, "themes"))

        # translations: the languages Thunar is translated to
        prefix_locale = self.pf("share", "locale")
        langs = sorted(l for l in os.listdir(prefix_locale)
                       if os.path.exists(os.path.join(prefix_locale, l, "LC_MESSAGES", "thunar.mo")))
        for lang in langs:
            dst = os.path.join(share, "locale", lang, "LC_MESSAGES")
            os.makedirs(dst, exist_ok=True)
            for base, domains in ((prefix_locale, PREFIX_DOMAINS), (self.hb("share", "locale"), HOMEBREW_DOMAINS)):
                for d in domains:
                    src = os.path.join(base, lang, "LC_MESSAGES", f"{d}.mo")
                    if os.path.exists(src):
                        shutil.copyfile(os.path.realpath(src), os.path.join(dst, f"{d}.mo"))
        log(f"  {len(langs)} languages")

        # Thunar data: Send To targets, with the helper found through PATH
        sendto_src = self.pf("share", "Thunar", "sendto")
        sendto_dst = os.path.join(share, "Thunar", "sendto")
        os.makedirs(sendto_dst, exist_ok=True)
        for f in os.listdir(sendto_src):
            with open(os.path.join(sendto_src, f)) as fp:
                text = fp.read()
            text = re.sub(r"^(Exec=)\S*/([^/\s]+)", r"\1\2", text, flags=re.M)
            with open(os.path.join(sendto_dst, f), "w") as fp:
                fp.write(text)

        # system configuration: default custom actions, xfconf defaults
        etc_xdg = self.pf("etc", "xdg")
        if os.path.isdir(etc_xdg):
            self.copytree(etc_xdg, os.path.join(self.resources, "etc", "xdg"))

        self.write_dbus_templates()

    def make_icon(self):
        svg = self.pf("share", "icons", "hicolor", "scalable", "apps", "org.xfce.thunar.svg")
        rsvg = self.hb("bin", "rsvg-convert")
        if not (os.path.exists(svg) and os.path.exists(rsvg)):
            log("warning: no rsvg-convert or Thunar SVG icon, the bundle has no icon")
            return
        with tempfile.TemporaryDirectory() as tmp:
            iconset = os.path.join(tmp, "Thunar.iconset")
            os.makedirs(iconset)
            for s in (16, 32, 128, 256, 512):
                run(rsvg, "-w", str(s), "-h", str(s), svg, "-o", os.path.join(iconset, f"icon_{s}x{s}.png"))
                run(rsvg, "-w", str(s * 2), "-h", str(s * 2), svg, "-o", os.path.join(iconset, f"icon_{s}x{s}@2x.png"))
            run("iconutil", "-c", "icns", iconset, "-o", os.path.join(self.resources, "Thunar.icns"))

    def write_dbus_templates(self):
        os.makedirs(os.path.join(self.runtime, "dbus-services"), exist_ok=True)
        with open(os.path.join(self.runtime, "dbus-session.conf"), "w") as fp:
            fp.write("""<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<!-- Private session bus of Thunar.app, generated by its launcher -->
<busconfig>
  <type>session</type>
  <keep_umask/>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <servicedir>@CACHE@/dbus-services</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
""")
        services = {
            "org.xfce.Xfconf": '"@CONTENTS@/MacOS/xfconfd"',
            "org.freedesktop.thumbnails.Thumbnailer1": '"@CONTENTS@/MacOS/thunar-macos-thumbnailer"',
            "org.freedesktop.thumbnails.Cache1": '"@CONTENTS@/MacOS/thunar-macos-thumbnailer"',
            "org.xfce.Thunar": '"@CONTENTS@/MacOS/Thunar" --gapplication-service',
            "org.xfce.FileManager": '"@CONTENTS@/MacOS/Thunar" --gapplication-service',
            "org.freedesktop.FileManager1": '"@CONTENTS@/MacOS/Thunar" --gapplication-service',
        }
        for name, exe in services.items():
            with open(os.path.join(self.runtime, "dbus-services", f"{name}.service"), "w") as fp:
                fp.write(f"[D-BUS Service]\nName={name}\nExec={exe}\n")

    # -- finishing -----------------------------------------------------------

    def write_library_list(self):
        """Contents/Resources/bundled-libraries.txt: where the bundled code comes from."""
        formulae = {}
        cellar = os.path.join(self.homebrew, "Cellar") + "/"
        for real in list(self.framework_names) + list(self.sources.values()):
            if real.startswith(cellar):
                name, version = real[len(cellar):].split("/")[:2]
                formulae[name] = version
        lines = ["Thunar.app bundles code from these sources.", "",
                 "Thunar: this repository (see the version in Info.plist).", "",
                 "Xfce libraries, built from git (revision, patches: macos/deps in the source):"]
        stamps = self.pf("share", "thunar-macos-deps")
        if os.path.isdir(stamps):
            for f in sorted(os.listdir(stamps)):
                with open(os.path.join(stamps, f)) as fp:
                    lines.append(f"  {f[:-len('.stamp')]} {fp.read().split()[0]}")
        lines += ["", "Homebrew formulae (bottles, source: https://formulae.brew.sh):"]
        lines += [f"  {n} {v}" for n, v in sorted(formulae.items())]
        with open(os.path.join(self.resources, "bundled-libraries.txt"), "w") as fp:
            fp.write("\n".join(lines) + "\n")

    def set_min_os(self):
        versions = []
        for path in self.all_machos():
            minos = load_commands(path)[3]
            if minos:
                versions.append(tuple(int(x) for x in minos.split(".")))
        minos = ".".join(str(x) for x in max(versions)) if versions else "11.0"
        run("plutil", "-replace", "LSMinimumSystemVersion", "-string", minos, os.path.join(self.contents, "Info.plist"))
        log(f"  LSMinimumSystemVersion {minos} (highest minos of the bundled binaries)")

    def all_machos(self):
        for root, _, files in os.walk(self.contents):
            for f in files:
                p = os.path.join(root, f)
                if is_macho(p):
                    yield p

    def sign(self):
        identity = self.identity
        log(f"signing ({'ad-hoc' if identity == '-' else identity})")
        opts = ["--force", "--sign", identity]
        if identity != "-":
            opts += ["--options", "runtime", "--timestamp"]
        main = os.path.join(self.macos, "Thunar")
        # inside-out: libraries and modules, then helpers, then the bundle
        files = sorted(self.all_machos(), key=lambda p: (not p.startswith(self.frameworks), p))
        for path in files:
            if path != main:
                ent = ["--entitlements", self.entitlements] if (self.entitlements and identity != "-"
                                                                and os.path.dirname(path) == self.macos) else []
                run("codesign", *opts, *ent, path)
        ent = ["--entitlements", self.entitlements] if (self.entitlements and identity != "-") else []
        run("codesign", *opts, *ent, self.app)

    def build(self):
        if not self.app.endswith(".app"):
            die(f"output must end in .app: {self.app}")
        if os.path.exists(self.app):
            if not os.path.exists(os.path.join(self.macos, "thunar-bin")):
                die(f"{self.app} exists and is not a Thunar.app, not touching it")
            shutil.rmtree(self.app)
        for d in (self.macos, self.frameworks, self.resources):
            os.makedirs(d)

        self.collect_binaries()
        self.collect_frameworks()
        self.fix_install_names()
        self.copy_data()
        self.write_library_list()
        self.set_min_os()
        self.sign()
        if verify(self.app) != 0:
            die("bundle verification failed")
        log(f"created {self.app} ({du(self.app)})")


def du(path):
    return run("du", "-sh", path).split()[0]


def verify(app):
    """Check that the bundle doesn't reference anything outside itself."""
    contents = os.path.join(os.path.realpath(app), "Contents")
    frameworks = os.path.join(contents, "Frameworks")
    problems = []
    n = 0
    for root, _, files in os.walk(contents):
        for f in files:
            path = os.path.join(root, f)
            if os.path.islink(path):
                target = os.path.realpath(path)
                if not target.startswith(contents + "/"):
                    problems.append(f"{path}: symlink to {target}")
                continue
            if not is_macho(path):
                continue
            n += 1
            ident, deps, rpaths, _ = load_commands(path)
            for dep in deps:
                if is_system(dep):
                    continue
                if not dep.startswith("@rpath/"):
                    problems.append(f"{path}: {dep}")
                elif not os.path.exists(os.path.join(frameworks, dep[len("@rpath/"):])):
                    problems.append(f"{path}: {dep} not in Frameworks")
            for rp in rpaths:
                if not rp.startswith(("@loader_path", "@executable_path")):
                    problems.append(f"{path}: LC_RPATH {rp}")
            if ident is not None and ident.startswith("/"):
                problems.append(f"{path}: install name {ident}")
    # text files must not point into the build machine (the runtime/ templates use placeholders)
    for root, _, files in os.walk(os.path.join(contents, "Resources")):
        for f in files:
            path = os.path.join(root, f)
            if f.endswith((".desktop", ".service", ".conf", ".cache", ".xml", ".theme")) and not f == "icon-theme.cache":
                with open(path, "rb") as fp:
                    data = fp.read()
                for bad in (b"/opt/homebrew", b"/usr/local/", b"/Users/", b"/opt/local"):
                    if bad in data:
                        problems.append(f"{path}: contains {bad.decode()}")
    res = subprocess.run(["codesign", "--verify", "--deep", "--strict", app], capture_output=True, text=True)
    if res.returncode != 0:
        problems.append(f"codesign --verify: {res.stderr.strip()}")
    for p in problems:
        print(f"  {p}", file=sys.stderr)
    log(f"verified {n} Mach-O files: {'OK' if not problems else f'{len(problems)} problems'}")
    return 1 if problems else 0


def main():
    here = os.path.dirname(os.path.realpath(__file__))
    installed_prefix = os.path.realpath(os.path.join(here, "..", "..", ".."))
    default_prefix = os.environ.get("XFCE_PREFIX") or (
        installed_prefix if os.path.exists(os.path.join(installed_prefix, "bin", "thunar")) else None)
    default_homebrew = os.environ.get("HOMEBREW_PREFIX") or (
        "/opt/homebrew" if os.path.isdir("/opt/homebrew") else "/usr/local")

    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--prefix", default=default_prefix, help="Thunar install prefix (default: %(default)s)")
    ap.add_argument("--homebrew", default=default_homebrew, help="Homebrew prefix (default: %(default)s)")
    ap.add_argument("--sign", default="-", metavar="IDENTITY",
                    help="codesign identity (default: ad-hoc); a real identity enables the hardened runtime")
    ap.add_argument("--entitlements", help="entitlements plist for the executables (only with --sign)")
    ap.add_argument("--verify", action="store_true", help="only verify an existing bundle")
    ap.add_argument("output", nargs="?", default=os.path.join(os.getcwd(), "Thunar.app"))
    args = ap.parse_args()

    if args.verify:
        sys.exit(verify(args.output))
    if not args.prefix:
        die("no prefix: use --prefix or set XFCE_PREFIX")
    Bundler(args).build()


if __name__ == "__main__":
    main()
