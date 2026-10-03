/*
 * Copyright (c) 2026 Thunar macOS port contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* Main executable of the self-contained Thunar.app (Contents/MacOS/Thunar).
 *
 * Everything Thunar needs is inside the bundle (see make-app-bundle.py):
 *
 *   Contents/MacOS/        this launcher, thunar-bin, xfconfd, dbus-daemon,
 *                          dbus-send and the Thunar helpers
 *   Contents/Frameworks/   all non-system dylibs (@rpath)
 *   Contents/Resources/    lib/ (thunarx plugins, gdk-pixbuf loaders, GTK
 *                          input methods), share/ (icons, schemas, locale,
 *                          Thunar data), etc/xdg/ and runtime/ (templates)
 *
 * Apps started from Finder/Dock get a bare environment and no D-Bus session
 * bus, so this launcher, with every path derived from the bundle location:
 *  - sets up the environment (XDG dirs, GSettings schemas, GIO modules,
 *    gdk-pixbuf loaders, GTK prefixes, thunarx plugins, translations, PATH,
 *    LANG from the macOS locale),
 *  - instantiates Contents/Resources/runtime/ into
 *    ~/Library/Caches/org.xfce.thunar/ (files that must contain absolute
 *    paths: loader caches, the bus configuration and D-Bus service files),
 *  - reuses the private session bus from the address file, or starts the
 *    bundled dbus-daemon and writes its address there (shared with
 *    macos/deps/with-dbus.sh, so both talk to the same Thunar/xfconfd),
 *  - redirects output to ~/Library/Logs/Thunar.log when not on a terminal
 *    (THUNAR_MACOS_LOG=<file> or "-" overrides that),
 *  - and execs Contents/MacOS/thunar-bin. Because that binary lives inside
 *    the bundle, macOS attributes the process (Dock icon, privacy
 *    permissions such as Full Disk Access) to Thunar.app.
 *
 * Plain C + CoreFoundation, no GLib: it must work before any environment
 * is set up. */

#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static char contents_dir[PATH_MAX];  /* .../Thunar.app/Contents */
static char resources_dir[PATH_MAX]; /* .../Thunar.app/Contents/Resources */
static char macos_dir[PATH_MAX];     /* .../Thunar.app/Contents/MacOS */
static char cache_dir[PATH_MAX];     /* ~/Library/Caches/org.xfce.thunar */



static void
setenv_printf (const char *name,
               const char *format,
               ...) __attribute__ ((format (printf, 2, 3)));

static void
setenv_printf (const char *name,
               const char *format,
               ...)
{
  char    buf[PATH_MAX * 4];
  va_list args;

  va_start (args, format);
  vsnprintf (buf, sizeof (buf), format, args);
  va_end (args);
  setenv (name, buf, 1);
}



static void
mkdir_p (const char *path)
{
  char  tmp[PATH_MAX];
  char *p;

  snprintf (tmp, sizeof (tmp), "%s", path);
  for (p = tmp + 1; *p != '\0'; ++p)
    if (*p == '/')
      {
        *p = '\0';
        mkdir (tmp, 0700);
        *p = '/';
      }
  mkdir (tmp, 0700);
}



static int
find_bundle (void)
{
  char     exe[PATH_MAX];
  char     real[PATH_MAX];
  uint32_t size = sizeof (exe);

  if (_NSGetExecutablePath (exe, &size) != 0 || realpath (exe, real) == NULL)
    return 0;

  /* real = .../Contents/MacOS/Thunar */
  snprintf (macos_dir, sizeof (macos_dir), "%s", dirname (real));
  snprintf (contents_dir, sizeof (contents_dir), "%s", macos_dir);
  snprintf (contents_dir, sizeof (contents_dir), "%s", dirname (contents_dir));
  snprintf (resources_dir, sizeof (resources_dir), "%s/Resources", contents_dir);

  return access (resources_dir, R_OK) == 0;
}



/* LANG from the macOS locale (apps started by launchd have none) */
static void
setup_lang (void)
{
  CFLocaleRef locale;
  CFStringRef ident;
  char        buf[64];

  if (getenv ("LANG") != NULL || getenv ("LC_ALL") != NULL)
    return;

  locale = CFLocaleCopyCurrent ();
  ident = CFLocaleGetIdentifier (locale);
  if (CFStringGetCString (ident, buf, sizeof (buf), kCFStringEncodingUTF8))
    {
      /* "en_US@rg=dezzzz" -> "en_US" */
      buf[strcspn (buf, "@")] = '\0';
      if (strchr (buf, '_') != NULL)
        setenv_printf ("LANG", "%s.UTF-8", buf);
    }
  CFRelease (locale);
}



static void
setup_env (void)
{
  const char *path = getenv ("PATH");
  const char *r = resources_dir;

  /* nothing from a development environment may leak into the bundle */
  static const char *unset[] = {
    "GIO_EXTRA_MODULES", "GTK_PATH", "GTK_MODULES", "GTK3_MODULES", "GTK_THEME", "GDK_PIXBUF_MODULEDIR",
    "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "DYLD_INSERT_LIBRARIES", "PKG_CONFIG_PATH",
  };
  for (size_t n = 0; n < sizeof (unset) / sizeof (unset[0]); ++n)
    unsetenv (unset[n]);

  setenv_printf ("XDG_DATA_DIRS", "%s/share", r);
  setenv_printf ("XDG_CONFIG_DIRS", "%s/etc/xdg", r);
  setenv_printf ("GSETTINGS_SCHEMA_DIR", "%s/share/glib-2.0/schemas", r);
  setenv_printf ("GIO_MODULE_DIR", "%s/lib/gio/modules", r);
  setenv_printf ("GDK_PIXBUF_MODULE_FILE", "%s/loaders.cache", cache_dir);
  setenv_printf ("GTK_IM_MODULE_FILE", "%s/immodules.cache", cache_dir);
  setenv_printf ("GTK_DATA_PREFIX", "%s", r);
  setenv_printf ("GTK_EXE_PREFIX", "%s", r);
  setenv_printf ("THUNARX_DIRS", "%s/lib/thunarx-3", r);
  setenv_printf ("XFCE_LOCALEDIR", "%s/share/locale", r);
  setenv_printf ("THUNAR_MACOS_BUNDLE", "%s", contents_dir);

  /* bundle helpers first (also makes "thunar" resolve to this launcher on
   * the case-insensitive file system); Homebrew last, for custom actions */
  setenv_printf ("PATH", "%s:%s:/opt/homebrew/bin:/usr/local/bin", macos_dir,
                 path != NULL && *path != '\0' ? path : "/usr/bin:/bin:/usr/sbin:/sbin");

  setup_lang ();
}



static char *
read_file (const char *path,
           size_t     *len_return)
{
  FILE  *fp = fopen (path, "rb");
  char  *data;
  long   len;

  if (fp == NULL)
    return NULL;
  fseek (fp, 0, SEEK_END);
  len = ftell (fp);
  fseek (fp, 0, SEEK_SET);
  data = malloc ((size_t) len + 1);
  if (data == NULL || fread (data, 1, (size_t) len, fp) != (size_t) len)
    {
      free (data);
      fclose (fp);
      return NULL;
    }
  data[len] = '\0';
  fclose (fp);
  *len_return = (size_t) len;
  return data;
}



/* replace @CONTENTS@, @RESOURCES@ and @CACHE@ */
static char *
substitute (const char *in)
{
  static const char *keys[] = { "@CONTENTS@", "@RESOURCES@", "@CACHE@" };
  const char        *values[] = { contents_dir, resources_dir, cache_dir };
  size_t             cap = strlen (in) * 2 + PATH_MAX * 4, len = 0;
  char              *out = malloc (cap);
  const char        *p = in;

  while (*p != '\0')
    {
      size_t k, n = 1;
      int    matched = 0;

      for (k = 0; k < 3; ++k)
        if (strncmp (p, keys[k], strlen (keys[k])) == 0)
          {
            size_t vlen = strlen (values[k]);
            if (len + vlen + 1 >= cap)
              out = realloc (out, cap = (cap + vlen) * 2);
            memcpy (out + len, values[k], vlen);
            len += vlen;
            n = strlen (keys[k]);
            matched = 1;
            break;
          }
      if (!matched)
        {
          if (len + 2 >= cap)
            out = realloc (out, cap *= 2);
          out[len++] = *p;
        }
      p += n;
    }
  out[len] = '\0';
  return out;
}



/* instantiate the templates in src into dst (only rewrite changed files, so
 * dbus-daemon doesn't reload unchanged service files) */
static void
instantiate_dir (const char *src,
                 const char *dst)
{
  DIR           *dp = opendir (src);
  struct dirent *de;

  if (dp == NULL)
    return;
  mkdir_p (dst);

  while ((de = readdir (dp)) != NULL)
    {
      char        spath[PATH_MAX], dpath[PATH_MAX];
      struct stat st;
      char       *data, *result, *existing;
      size_t      len, elen;

      if (de->d_name[0] == '.')
        continue;
      snprintf (spath, sizeof (spath), "%s/%s", src, de->d_name);
      snprintf (dpath, sizeof (dpath), "%s/%s", dst, de->d_name);
      if (stat (spath, &st) != 0)
        continue;
      if (S_ISDIR (st.st_mode))
        {
          instantiate_dir (spath, dpath);
          continue;
        }

      data = read_file (spath, &len);
      if (data == NULL)
        continue;
      result = substitute (data);
      existing = read_file (dpath, &elen);
      if (existing == NULL || strcmp (existing, result) != 0)
        {
          char  tmp[PATH_MAX + 8];
          FILE *fp;

          snprintf (tmp, sizeof (tmp), "%s.tmp", dpath);
          fp = fopen (tmp, "wb");
          if (fp != NULL)
            {
              fputs (result, fp);
              fclose (fp);
              rename (tmp, dpath);
            }
        }
      free (existing);
      free (result);
      free (data);
    }
  closedir (dp);
}



/* run argv, return the exit status (-1 on spawn failure) */
static int
run (char *const argv[])
{
  pid_t pid;
  int   status;

  if (posix_spawn (&pid, argv[0], NULL, NULL, argv, environ) != 0)
    return -1;
  while (waitpid (pid, &status, 0) < 0)
    if (errno != EINTR)
      return -1;
  return WIFEXITED (status) ? WEXITSTATUS (status) : -1;
}



static int
bus_is_alive (const char *address)
{
  char  dbus_send[PATH_MAX];
  char  arg[1024];
  char *argv[] = { dbus_send, arg, "--dest=org.freedesktop.DBus", "--print-reply", "/",
                   "org.freedesktop.DBus.GetId", NULL };
  int   devnull, saved_out, saved_err, result;

  snprintf (dbus_send, sizeof (dbus_send), "%s/dbus-send", macos_dir);
  snprintf (arg, sizeof (arg), "--bus=%s", address);

  /* silence dbus-send */
  fflush (stdout);
  fflush (stderr);
  saved_out = dup (STDOUT_FILENO);
  saved_err = dup (STDERR_FILENO);
  devnull = open ("/dev/null", O_WRONLY);
  dup2 (devnull, STDOUT_FILENO);
  dup2 (devnull, STDERR_FILENO);
  result = run (argv);
  dup2 (saved_out, STDOUT_FILENO);
  dup2 (saved_err, STDERR_FILENO);
  close (saved_out);
  close (saved_err);
  close (devnull);

  return result == 0;
}



static int
read_address (const char *file,
              char       *address,
              size_t      len)
{
  FILE *fp = fopen (file, "r");
  if (fp == NULL)
    return 0;
  if (fgets (address, (int) len, fp) == NULL)
    address[0] = '\0';
  fclose (fp);
  address[strcspn (address, "\r\n")] = '\0';
  return address[0] != '\0';
}



static int
start_bus (const char *file,
           char       *address,
           size_t      len)
{
  char  dbus_daemon[PATH_MAX];
  char  config[PATH_MAX + 16];
  char *argv[] = { dbus_daemon, config, "--fork", "--print-address=1", NULL };
  int   fd, saved_out, result;

  snprintf (dbus_daemon, sizeof (dbus_daemon), "%s/dbus-daemon", macos_dir);
  snprintf (config, sizeof (config), "--config-file=%s/dbus-session.conf", cache_dir);

  /* dbus-daemon prints the address on stdout -> the address file */
  fd = open (file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    return 0;
  fflush (stdout);
  saved_out = dup (STDOUT_FILENO);
  dup2 (fd, STDOUT_FILENO);
  close (fd);
  result = run (argv);
  dup2 (saved_out, STDOUT_FILENO);
  close (saved_out);

  return result == 0 && read_address (file, address, len);
}



static void
setup_dbus (void)
{
  const char *current = getenv ("DBUS_SESSION_BUS_ADDRESS");
  char        file[PATH_MAX];
  char        dir[PATH_MAX];
  char        address[1024];

  /* started from a shell that already has our bus */
  if (current != NULL && *current != '\0' && bus_is_alive (current))
    return;

  if (getenv ("THUNAR_DBUS_ADDRESS_FILE") != NULL)
    snprintf (file, sizeof (file), "%s", getenv ("THUNAR_DBUS_ADDRESS_FILE"));
  else
    snprintf (file, sizeof (file), "%s/dbus-session-address", cache_dir);

  snprintf (dir, sizeof (dir), "%s", file);
  mkdir_p (dirname (dir));

  if (!(read_address (file, address, sizeof (address)) && bus_is_alive (address))
      && !start_bus (file, address, sizeof (address)))
    {
      fprintf (stderr, "Thunar.app: failed to start %s/dbus-daemon\n", macos_dir);
      unsetenv ("DBUS_SESSION_BUS_ADDRESS");
      return;
    }

  setenv ("DBUS_SESSION_BUS_ADDRESS", address, 1);
}



static void
setup_log (void)
{
  const char *home = getenv ("HOME");
  const char *log = getenv ("THUNAR_MACOS_LOG"); /* a file, or "-" for no redirection */
  char        path[PATH_MAX];
  int         fd;

  if (log != NULL && *log != '\0')
    {
      if (strcmp (log, "-") == 0)
        return;
      snprintf (path, sizeof (path), "%s", log);
    }
  else
    {
      if (isatty (STDERR_FILENO) || home == NULL)
        return;
      snprintf (path, sizeof (path), "%s/Library/Logs", home);
      mkdir_p (path);
      strlcat (path, "/Thunar.log", sizeof (path));
    }
  fd = open (path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0)
    {
      dup2 (fd, STDOUT_FILENO);
      dup2 (fd, STDERR_FILENO);
      close (fd);
    }
}



static int
is_info_option (const char *arg)
{
  return strcmp (arg, "--version") == 0 || strcmp (arg, "-V") == 0 || strcmp (arg, "--help") == 0
         || strcmp (arg, "-h") == 0 || strncmp (arg, "--help-", 7) == 0;
}



int
main (int    argc,
      char **argv)
{
  const char *home = getenv ("HOME");
  char        target[PATH_MAX];
  char        templates[PATH_MAX];
  char      **args;
  int         n, i, info_only = 0;

  if (!find_bundle ())
    {
      fprintf (stderr, "Thunar.app: cannot locate the bundle resources\n");
      return EXIT_FAILURE;
    }

  for (i = 1; i < argc; ++i)
    if (is_info_option (argv[i]))
      info_only = 1;

  if (!info_only)
    setup_log ();

  snprintf (cache_dir, sizeof (cache_dir), "%s/Library/Caches/org.xfce.thunar", home != NULL ? home : "/tmp");
  if (getenv ("THUNAR_MACOS_CACHE_DIR") != NULL)
    snprintf (cache_dir, sizeof (cache_dir), "%s", getenv ("THUNAR_MACOS_CACHE_DIR"));
  mkdir_p (cache_dir);

  snprintf (templates, sizeof (templates), "%s/runtime", resources_dir);
  instantiate_dir (templates, cache_dir);

  setup_env ();
  if (!info_only)
    setup_dbus ();
  else
    setenv ("DBUS_SESSION_BUS_ADDRESS", "disabled:", 1); /* no bus, no autolaunch */

  snprintf (target, sizeof (target), "%s/thunar-bin", macos_dir);

  /* drop the legacy -psn_ argument, open $HOME when started without files
   * (the working directory of apps started from Finder is /) */
  args = calloc ((size_t) argc + 2, sizeof (char *));
  args[0] = target;
  for (n = 1, i = 1; i < argc; ++i)
    if (strncmp (argv[i], "-psn_", 5) != 0)
      args[n++] = argv[i];
  if (n == 1 && home != NULL)
    {
      if (chdir (home) == 0)
        args[n++] = (char *) home;
      /* tells thunar to reuse this window for files passed in the launch's
       * Apple "odoc" event (Finder "Open With", drop on the Dock icon) */
      setenv ("THUNAR_MACOS_APP_LAUNCH", "1", 1);
    }
  args[n] = NULL;

  execv (target, args);
  fprintf (stderr, "Thunar.app: failed to execute %s: %s\n", target, strerror (errno));
  return EXIT_FAILURE;
}
