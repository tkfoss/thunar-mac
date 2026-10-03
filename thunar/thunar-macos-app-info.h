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

#ifndef __THUNAR_MACOS_APP_INFO_H__
#define __THUNAR_MACOS_APP_INFO_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#ifdef __APPLE__

/* A GAppInfo for macOS, replacing the very limited GOSXAppInfo and the
 * missing GDesktopAppInfo:
 *  - "bundle" apps (.app) via LaunchServices: listing per content type,
 *    system wide defaults, launching, Finder icons
 *  - "command" apps from .desktop key files (custom commands from Open With,
 *    Send To targets, .desktop files in $XDG_DATA_DIRS/applications), spawned
 *    with %f/%F/%u/%U expansion.
 * Associations that LaunchServices can't store (last used app, added/removed
 * apps, command apps as default) are kept in ~/.config/Thunar/macos-mimeapps.list
 * keyed by content type (UTI). */

#define THUNAR_TYPE_MACOS_APP_INFO (thunar_macos_app_info_get_type ())
G_DECLARE_FINAL_TYPE (ThunarMacosAppInfo, thunar_macos_app_info, THUNAR, MACOS_APP_INFO, GObject)

GAppInfo *
thunar_macos_app_info_new_for_bundle (const gchar *app_path);
GAppInfo *
thunar_macos_app_info_new_from_keyfile (GKeyFile    *key_file,
                                        const gchar *filename);
const gchar *const *
thunar_macos_app_info_get_mime_types (ThunarMacosAppInfo *info);

/* registry, mirroring the g_app_info_* API (see thunar-app-info.h) */
GAppInfo *
thunar_macos_app_info_create_from_commandline (const gchar *commandline,
                                               const gchar *application_name,
                                               GError     **error);
GList *
thunar_macos_app_info_get_all (void);
GList *
thunar_macos_app_info_get_all_for_type (const gchar *content_type);
GList *
thunar_macos_app_info_get_recommended_for_type (const gchar *content_type);
GAppInfo *
thunar_macos_app_info_get_default_for_type (const gchar *content_type,
                                            gboolean     must_support_uris);
gboolean
thunar_macos_app_info_set_as_default_for_type (GAppInfo    *info,
                                               const gchar *content_type,
                                               GError     **error);
gboolean
thunar_macos_app_info_set_as_last_used_for_type (GAppInfo    *info,
                                                 const gchar *content_type,
                                                 GError     **error);

#endif /* __APPLE__ */

G_END_DECLS

#endif /* !__THUNAR_MACOS_APP_INFO_H__ */
