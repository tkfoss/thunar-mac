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

/* Support for running from a relocatable Thunar.app.
 *
 * The launcher (macos/thunar-app-launcher.c) points XFCE_LOCALEDIR at the
 * bundle's share/locale. Thunar, its plugins and libxfce4ui use it for their
 * own text domains (PACKAGE_LOCALE_DIR), but GLib, GTK and libxfce4util bind
 * theirs to their compiled-in prefix, lazily, on first use. Rebind those
 * after they initialized. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "thunar/thunar-macos.h"

#include <glib/gi18n-lib.h>
#include <libxfce4util/libxfce4util.h>

/* exported by GLib, initializes its text domain */
extern const gchar *
glib_gettext (const gchar *str);



void
thunar_macos_bind_textdomains (void)
{
  static const gchar *domains[] = { "glib20", "gtk30", "gtk30-properties", "libxfce4util", "xfconf" };
  const gchar        *localedir = g_getenv ("XFCE_LOCALEDIR");
  guint               n;

  if (localedir == NULL || *localedir == '\0')
    return;

  /* trigger the lazy bindtextdomain() of GLib and libxfce4util; GTK binds
   * in gtk_init(), so this must be called after that */
  (void) glib_gettext ("");
  xfce_get_license_text (XFCE_LICENSE_TEXT_BSD);

  for (n = 0; n < G_N_ELEMENTS (domains); ++n)
    {
      bindtextdomain (domains[n], localedir);
      bind_textdomain_codeset (domains[n], "UTF-8");
    }
}
