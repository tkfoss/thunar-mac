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

#ifndef __THUNAR_MACOS_H__
#define __THUNAR_MACOS_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#ifdef __APPLE__

/* thunar-macos-utils.m: thin wrappers around Foundation / BSD APIs */

/* Moves @file to the macOS Trash (NSFileManager) and records the original
 * path and deletion time as xattrs, so it can be restored later. */
gboolean thunar_macos_trash_file (GFile        *file,
                                  GCancellable *cancellable,
                                  GError      **error);

/* Path of the home trash (~/.Trash, or $THUNAR_MACOS_TRASH_DIR for testing) */
const gchar *thunar_macos_home_trash_dir (void);

/* NULL-terminated list of existing trash directories:
 * home trash first, then /Volumes/<vol>/.Trashes/<uid> */
gchar **thunar_macos_trash_dirs (void);

/* Original absolute path of a top-level trashed item, from our xattr or
 * from Finder's "Put Back" data in the trash's .DS_Store. May return NULL. */
gchar *thunar_macos_trash_get_orig_path (const gchar *trash_dir,
                                         const gchar *item_path);

/* Deletion time (unix seconds) of a top-level trashed item */
gint64 thunar_macos_trash_get_deletion_time (const gchar *item_path);

/* Forget the trash metadata of an item (after it was restored) */
void thunar_macos_trash_clear_metadata (const gchar *path);

/* Recursively delete @path (like rm -rf, never follows symlinks) */
gboolean thunar_macos_remove_recursive (const gchar *path,
                                        GError     **error);

/* thunar-macos-launch.m: LaunchServices / NSWorkspace */

/* Called in the GTK main loop once LaunchServices finished (error is NULL on
 * success and owned by the caller of the callback) */
typedef void (*ThunarMacosLaunchCallback) (GError  *error,
                                           gpointer user_data);

/* NULL-terminated bundle paths of the apps that can open @content_type (UTI) */
gchar **thunar_macos_apps_for_content_type (const gchar *content_type);

/* Bundle path of the default app for @content_type, or NULL */
gchar *thunar_macos_default_app_for_content_type (const gchar *content_type);

/* Make @app_path the system wide default app for @content_type */
gboolean thunar_macos_set_default_app_for_content_type (const gchar *app_path,
                                                        const gchar *content_type,
                                                        GError     **error);

/* Bundle path for a bundle identifier, or NULL */
gchar *thunar_macos_app_path_for_bundle_id (const gchar *bundle_id);

/* Bundle id, localized display name and main executable of an app bundle */
gboolean thunar_macos_app_bundle_info (const gchar *app_path,
                                       gchar      **bundle_id,
                                       gchar      **display_name,
                                       gchar      **executable);

/* NULL-terminated bundle paths of all apps in the standard locations */
gchar **thunar_macos_all_apps (void);

/* Finder icon of @path (app, file or folder) rendered as @size px PNG */
GBytes *thunar_macos_file_icon_png (const gchar *path,
                                    gint         size);

/* Open @uris (NULL-terminated, may be NULL to just launch) with the app
 * bundle at @app_path. Asynchronous, @callback may be NULL. */
gboolean thunar_macos_open_with_app (const gchar              *app_path,
                                     const gchar *const       *uris,
                                     ThunarMacosLaunchCallback callback,
                                     gpointer                  user_data);

/* thunar-macos-volumes.m: volume metadata, unmount/eject */

/* Localized name, UUID (may be NULL), ejectable/removable and local flags
 * of the volume mounted at @mount_path */
gboolean thunar_macos_volume_info (const gchar *mount_path,
                                   gchar      **name,
                                   gchar      **uuid,
                                   gboolean    *ejectable,
                                   gboolean    *local);

/* Unmount (and eject the whole disk if @eject) asynchronously */
void thunar_macos_volume_unmount (const gchar              *mount_path,
                                  gboolean                  eject,
                                  ThunarMacosLaunchCallback callback,
                                  gpointer                  user_data);

/* thunar-macos-pasteboard.m: files on the general pasteboard (Finder
 * compatible, one public.file-url item per file) and in drags */

/* Current changeCount of the general pasteboard */
glong thunar_macos_pasteboard_change_count (void);

/* Put @files (GFiles) on the pasteboard, returns the new changeCount */
glong thunar_macos_pasteboard_write_files (GList   *files,
                                          gboolean cut);

/* GFiles on the pasteboard (free with thunar_g_list_free_full), @cut tells
 * whether Thunar cut them */
GList *thunar_macos_pasteboard_read_files (gboolean *cut);

/* Whether the pasteboard contains files */
gboolean thunar_macos_pasteboard_has_files (void);

/* MIME type of an image on the pasteboard that is not a file, or NULL */
gchar *thunar_macos_pasteboard_image_mime_type (void);

/* Data of the image with @mime_type on the pasteboard, or NULL */
GBytes *thunar_macos_pasteboard_read_image (const gchar *mime_type);

/* Clear the pasteboard if its changeCount is still @change_count */
void thunar_macos_pasteboard_clear (glong change_count);

/* GFiles that the next drag from Thunar carries (NULL when it ended) */
void thunar_macos_drag_set_files (GList *files);

/* thunar-macos-volume-monitor.c: GIO native volume monitor for /Volumes */
void thunar_macos_volume_monitor_register (void);

/* thunar-macos-trash.c: trash:// URI scheme backed by the macOS Trash */
void thunar_macos_trash_vfs_register (void);

/* relocatable Thunar.app: bind library text domains to $XFCE_LOCALEDIR */
void thunar_macos_bind_textdomains (void);

/* thunar-macos-launch.m: track NSApplicationDidFinishLaunchingNotification,
 * which Cocoa posts after delivering the files the app was launched with */
void thunar_macos_watch_launch (void);
gboolean thunar_macos_finished_launching (void);

#endif /* __APPLE__ */

G_END_DECLS

#endif /* !__THUNAR_MACOS_H__ */
