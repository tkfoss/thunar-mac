/*
 * Copyright (c) 2026 Thunar macOS port contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

/* Headless check of a Thunar.app bundle, run by test-bundle.sh in place of
 * Contents/MacOS/thunar-bin (in a copy of the bundle), so it gets exactly the
 * environment and session bus the launcher sets up. No window, no GTK init.
 *
 * Checks: gdk-pixbuf loaders (SVG), GSettings schemas, translations,
 * thunarx plugins, xfconf via the bus (activates xfconfd), the thumbnailer
 * (D-Bus activation) and that no image is loaded from outside the bundle
 * or the system. */

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <libintl.h>
#include <locale.h>
#include <mach-o/dyld.h>
#include <string.h>
#include <thunarx/thunarx.h>
#include <xfconf/xfconf.h>

static int failures = 0;

#define CHECK(cond, ...) \
  G_STMT_START \
  { \
    if (cond) \
      g_print ("ok    "); \
    else \
      { \
        g_print ("FAIL  "); \
        failures++; \
      } \
    g_print (__VA_ARGS__); \
    g_print ("\n"); \
  } \
  G_STMT_END

int
main (int argc, char **argv)
{
  const gchar            *contents = g_getenv ("THUNAR_MACOS_BUNDLE");
  const gchar            *localedir = g_getenv ("XFCE_LOCALEDIR");
  gchar                  *resources, *svg, *s;
  GdkPixbuf              *pixbuf;
  GError                 *error = NULL;
  GSList                 *formats, *lp;
  gboolean                have_svg = FALSE;
  GSettingsSchema        *schema;
  ThunarxProviderFactory *factory;
  GList                  *providers;
  XfconfChannel          *channel;
  GDBusConnection        *bus;
  GVariant               *reply;
  guint                   n;

  CHECK (contents != NULL, "THUNAR_MACOS_BUNDLE=%s", contents);
  if (contents == NULL)
    return 1;
  resources = g_build_filename (contents, "Resources", NULL);

  /* gdk-pixbuf loaders from the instantiated loaders.cache */
  formats = gdk_pixbuf_get_formats ();
  for (lp = formats; lp != NULL; lp = lp->next)
    if (g_strcmp0 (gdk_pixbuf_format_get_name (lp->data), "svg") == 0)
      have_svg = TRUE;
  g_slist_free (formats);
  CHECK (have_svg, "gdk-pixbuf has the SVG loader (GDK_PIXBUF_MODULE_FILE=%s)", g_getenv ("GDK_PIXBUF_MODULE_FILE"));
  svg = g_build_filename (resources, "share/icons/Adwaita/scalable/places/folder.svg", NULL);
  pixbuf = gdk_pixbuf_new_from_file_at_size (svg, 64, 64, &error);
  CHECK (pixbuf != NULL, "render %s: %s", svg, error ? error->message : "64x64");
  g_clear_error (&error);
  g_clear_object (&pixbuf);

  /* GSettings */
  schema = g_settings_schema_source_lookup (g_settings_schema_source_get_default (), "org.gtk.Settings.FileChooser", TRUE);
  CHECK (schema != NULL, "GSettings schema org.gtk.Settings.FileChooser");
  if (schema != NULL)
    g_settings_schema_unref (schema);

  /* translations (LANG=de_DE.UTF-8 is set by test-bundle.sh) */
  setlocale (LC_ALL, "");
  bindtextdomain ("thunar", localedir);
  bind_textdomain_codeset ("thunar", "UTF-8");
  bindtextdomain ("gtk30", localedir);
  bind_textdomain_codeset ("gtk30", "UTF-8");
  CHECK (strcmp (dgettext ("thunar", "_Edit"), "_Bearbeiten") == 0, "thunar translation: _Edit -> %s", dgettext ("thunar", "_Edit"));
  CHECK (strcmp (dgettext ("gtk30", "_Cancel"), "_Cancel") != 0, "gtk30 translation: _Cancel -> %s", dgettext ("gtk30", "_Cancel"));

  /* thunarx plugins from THUNARX_DIRS */
  factory = thunarx_provider_factory_get_default ();
  providers = thunarx_provider_factory_list_providers (factory, THUNARX_TYPE_PROPERTY_PAGE_PROVIDER);
  CHECK (providers != NULL, "thunarx property page providers: %u (THUNARX_DIRS=%s)", g_list_length (providers), g_getenv ("THUNARX_DIRS"));
  g_list_free_full (providers, g_object_unref);
  providers = thunarx_provider_factory_list_providers (factory, THUNARX_TYPE_RENAMER_PROVIDER);
  CHECK (providers != NULL, "thunarx renamer providers: %u", g_list_length (providers));
  g_list_free_full (providers, g_object_unref);

  /* xfconf through the private bus (D-Bus activates the bundled xfconfd) */
  CHECK (xfconf_init (&error), "xfconf_init: %s", error ? error->message : g_getenv ("DBUS_SESSION_BUS_ADDRESS"));
  g_clear_error (&error);
  channel = xfconf_channel_get ("thunar-bundle-test");
  xfconf_channel_set_string (channel, "/test", "value");
  s = xfconf_channel_get_string (channel, "/test", NULL);
  CHECK (g_strcmp0 (s, "value") == 0, "xfconf round trip: %s", s);
  g_free (s);
  xfconf_channel_reset_property (channel, "/test", FALSE);
  xfconf_shutdown ();

  /* the thumbnailer (D-Bus activation) */
  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  reply = bus == NULL ? NULL
                      : g_dbus_connection_call_sync (bus, "org.freedesktop.thumbnails.Thumbnailer1",
                                                     "/org/freedesktop/thumbnails/Thumbnailer1",
                                                     "org.freedesktop.thumbnails.Thumbnailer1", "GetSupported", NULL,
                                                     NULL, G_DBUS_CALL_FLAGS_NONE, 20000, NULL, &error);
  CHECK (reply != NULL, "thumbnailer GetSupported: %s", error ? error->message : "replied");
  g_clear_error (&error);
  if (reply != NULL)
    g_variant_unref (reply);
  g_clear_object (&bus);

  /* nothing loaded from outside the bundle */
  for (n = 0; n < _dyld_image_count (); ++n)
    {
      const char *name = _dyld_get_image_name (n);
      gchar      *real = realpath (name, NULL);
      gchar      *creal = realpath (contents, NULL);
      gboolean    inside = real != NULL && creal != NULL && g_str_has_prefix (real, creal);
      if (!inside && !g_str_has_prefix (name, "/usr/lib/") && !g_str_has_prefix (name, "/System/"))
        CHECK (FALSE, "image outside the bundle: %s", name);
      free (real);
      free (creal);
    }
  g_print ("info  %u images loaded, all from the bundle or the system unless listed above\n", _dyld_image_count ());

  g_free (svg);
  g_free (resources);
  g_print ("%s\n", failures == 0 ? "PASS" : "FAILED");
  return failures == 0 ? 0 : 1;
}
