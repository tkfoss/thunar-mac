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

/* GLib has no native volume monitor on macOS, so g_volume_monitor_get()
 * reports no mounts at all and Thunar can't show or eject USB disks, disk
 * images or network shares. This native monitor provides a GMount for every
 * browsable volume under /Volumes (what Finder lists under Locations),
 * polling the mount table, with Finder icons and unmount/eject through
 * NSFileManager. There are no GVolumes/GDrives: macOS mounts automatically. */

/* getmntinfo() and struct statfs need the BSD types hidden by _XOPEN_SOURCE */
#define _DARWIN_C_SOURCE 1

#include <gio/gio.h>
#include <gio/gnativevolumemonitor.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>

#include "thunar/thunar-macos.h"

#define POLL_INTERVAL_SECONDS 2



/* ---- ThunarMacosMount -------------------------------------------------- */

#define THUNAR_TYPE_MACOS_MOUNT (thunar_macos_mount_get_type ())
G_DECLARE_FINAL_TYPE (ThunarMacosMount, thunar_macos_mount, THUNAR, MACOS_MOUNT, GObject)

struct _ThunarMacosMount
{
  GObject         __parent__;
  GVolumeMonitor *monitor; /* weak */
  gchar          *path;
  gchar          *name;
  gchar          *uuid;
  gboolean        ejectable;
  gboolean        local;
  GIcon          *icon;
};

static void
thunar_macos_mount_iface_init (GMountIface *iface);

G_DEFINE_TYPE_WITH_CODE (ThunarMacosMount, thunar_macos_mount, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT, thunar_macos_mount_iface_init))

static void
thunar_macos_volume_monitor_refresh (GVolumeMonitor *monitor);



static void
thunar_macos_mount_finalize (GObject *object)
{
  ThunarMacosMount *mount = THUNAR_MACOS_MOUNT (object);

  if (mount->monitor != NULL)
    g_object_remove_weak_pointer (G_OBJECT (mount->monitor), (gpointer *) &mount->monitor);
  g_free (mount->path);
  g_free (mount->name);
  g_free (mount->uuid);
  g_clear_object (&mount->icon);

  G_OBJECT_CLASS (thunar_macos_mount_parent_class)->finalize (object);
}



static void
thunar_macos_mount_class_init (ThunarMacosMountClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = thunar_macos_mount_finalize;
}



static void
thunar_macos_mount_init (ThunarMacosMount *mount)
{
}



static ThunarMacosMount *
thunar_macos_mount_new (GVolumeMonitor *monitor,
                        const gchar    *path)
{
  ThunarMacosMount *mount = g_object_new (THUNAR_TYPE_MACOS_MOUNT, NULL);

  mount->monitor = monitor;
  g_object_add_weak_pointer (G_OBJECT (monitor), (gpointer *) &mount->monitor);
  mount->path = g_strdup (path);
  mount->local = TRUE;
  if (!thunar_macos_volume_info (path, &mount->name, &mount->uuid, &mount->ejectable, &mount->local))
    mount->ejectable = FALSE;
  if (mount->name == NULL)
    mount->name = g_path_get_basename (path);

  return mount;
}



static GFile *
thunar_macos_mount_get_root (GMount *mount)
{
  return g_file_new_for_path (THUNAR_MACOS_MOUNT (mount)->path);
}



static gchar *
thunar_macos_mount_get_name (GMount *mount)
{
  return g_strdup (THUNAR_MACOS_MOUNT (mount)->name);
}



static const gchar *
thunar_macos_mount_symbolic_icon_name (ThunarMacosMount *mount)
{
  if (!mount->local)
    return "folder-remote";
  return mount->ejectable ? "drive-removable-media" : "drive-harddisk";
}



static GIcon *
thunar_macos_mount_get_icon (GMount *gmount)
{
  ThunarMacosMount *mount = THUNAR_MACOS_MOUNT (gmount);
  GBytes           *png;

  /* the Finder icon of the volume (USB stick, disk image, server, ...) */
  if (mount->icon == NULL)
    {
      png = thunar_macos_file_icon_png (mount->path, 256);
      if (png != NULL)
        {
          mount->icon = g_bytes_icon_new (png);
          g_bytes_unref (png);
        }
      else
        mount->icon = g_themed_icon_new_with_default_fallbacks (thunar_macos_mount_symbolic_icon_name (mount));
    }

  return g_object_ref (mount->icon);
}



static GIcon *
thunar_macos_mount_get_symbolic_icon (GMount *mount)
{
  gchar *name = g_strconcat (thunar_macos_mount_symbolic_icon_name (THUNAR_MACOS_MOUNT (mount)), "-symbolic", NULL);
  GIcon *icon = g_themed_icon_new_with_default_fallbacks (name);
  g_free (name);
  return icon;
}



static gchar *
thunar_macos_mount_get_uuid (GMount *mount)
{
  return g_strdup (THUNAR_MACOS_MOUNT (mount)->uuid);
}



static GVolume *
thunar_macos_mount_get_volume (GMount *mount)
{
  return NULL;
}



static GDrive *
thunar_macos_mount_get_drive (GMount *mount)
{
  return NULL;
}



static gboolean
thunar_macos_mount_can_unmount (GMount *mount)
{
  return TRUE;
}



static gboolean
thunar_macos_mount_can_eject (GMount *mount)
{
  return THUNAR_MACOS_MOUNT (mount)->ejectable;
}



static const gchar *
thunar_macos_mount_get_sort_key (GMount *mount)
{
  return THUNAR_MACOS_MOUNT (mount)->name;
}



static void
thunar_macos_mount_unmount_done (GError  *error,
                                 gpointer user_data)
{
  GTask            *task = G_TASK (user_data);
  ThunarMacosMount *mount = g_task_get_source_object (task);

  if (error != NULL)
    {
      g_task_return_error (task, g_error_copy (error));
    }
  else
    {
      /* don't wait for the next poll to drop the mount */
      if (mount->monitor != NULL)
        thunar_macos_volume_monitor_refresh (mount->monitor);
      g_task_return_boolean (task, TRUE);
    }

  g_object_unref (task);
}



static void
thunar_macos_mount_unmount_common (GMount             *gmount,
                                   gboolean            eject,
                                   GCancellable       *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer            user_data)
{
  ThunarMacosMount *mount = THUNAR_MACOS_MOUNT (gmount);
  GTask            *task = g_task_new (gmount, cancellable, callback, user_data);

  /* let views release the files on the volume first */
  if (mount->monitor != NULL)
    g_signal_emit_by_name (mount->monitor, "mount-pre-unmount", gmount);
  g_signal_emit_by_name (gmount, "pre-unmount");

  thunar_macos_volume_unmount (mount->path, eject, thunar_macos_mount_unmount_done, task);
}



static void
thunar_macos_mount_unmount_with_operation (GMount             *mount,
                                           GMountUnmountFlags  flags,
                                           GMountOperation    *mount_operation,
                                           GCancellable       *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer            user_data)
{
  thunar_macos_mount_unmount_common (mount, FALSE, cancellable, callback, user_data);
}



static void
thunar_macos_mount_unmount (GMount             *mount,
                            GMountUnmountFlags  flags,
                            GCancellable       *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer            user_data)
{
  thunar_macos_mount_unmount_common (mount, FALSE, cancellable, callback, user_data);
}



static void
thunar_macos_mount_eject_with_operation (GMount             *mount,
                                         GMountUnmountFlags  flags,
                                         GMountOperation    *mount_operation,
                                         GCancellable       *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer            user_data)
{
  thunar_macos_mount_unmount_common (mount, TRUE, cancellable, callback, user_data);
}



static void
thunar_macos_mount_eject (GMount             *mount,
                          GMountUnmountFlags  flags,
                          GCancellable       *cancellable,
                          GAsyncReadyCallback callback,
                          gpointer            user_data)
{
  thunar_macos_mount_unmount_common (mount, TRUE, cancellable, callback, user_data);
}



static gboolean
thunar_macos_mount_finish (GMount       *mount,
                           GAsyncResult *result,
                           GError      **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}



static void
thunar_macos_mount_iface_init (GMountIface *iface)
{
  iface->get_root = thunar_macos_mount_get_root;
  iface->get_name = thunar_macos_mount_get_name;
  iface->get_icon = thunar_macos_mount_get_icon;
  iface->get_symbolic_icon = thunar_macos_mount_get_symbolic_icon;
  iface->get_uuid = thunar_macos_mount_get_uuid;
  iface->get_volume = thunar_macos_mount_get_volume;
  iface->get_drive = thunar_macos_mount_get_drive;
  iface->can_unmount = thunar_macos_mount_can_unmount;
  iface->can_eject = thunar_macos_mount_can_eject;
  iface->get_sort_key = thunar_macos_mount_get_sort_key;
  iface->unmount = thunar_macos_mount_unmount;
  iface->unmount_finish = thunar_macos_mount_finish;
  iface->unmount_with_operation = thunar_macos_mount_unmount_with_operation;
  iface->unmount_with_operation_finish = thunar_macos_mount_finish;
  iface->eject = thunar_macos_mount_eject;
  iface->eject_finish = thunar_macos_mount_finish;
  iface->eject_with_operation = thunar_macos_mount_eject_with_operation;
  iface->eject_with_operation_finish = thunar_macos_mount_finish;
}



/* ---- ThunarMacosVolumeMonitor ------------------------------------------ */

#define THUNAR_TYPE_MACOS_VOLUME_MONITOR (thunar_macos_volume_monitor_get_type ())
G_DECLARE_FINAL_TYPE (ThunarMacosVolumeMonitor, thunar_macos_volume_monitor, THUNAR, MACOS_VOLUME_MONITOR, GNativeVolumeMonitor)

struct _ThunarMacosVolumeMonitor
{
  GNativeVolumeMonitor __parent__;
  GHashTable          *mounts; /* mount path -> ThunarMacosMount */
  guint                poll_id;
};

G_DEFINE_TYPE (ThunarMacosVolumeMonitor, thunar_macos_volume_monitor, G_TYPE_NATIVE_VOLUME_MONITOR)

/* the instance, for the get_mount_for_mount_path class method */
static ThunarMacosVolumeMonitor *the_monitor = NULL;



/* volumes Finder shows: below /Volumes and not flagged "don't browse"
 * (that excludes the system/data/preboot volumes) */
static gboolean
mount_is_user_visible (const struct statfs *fs)
{
  return g_str_has_prefix (fs->f_mntonname, "/Volumes/")
         && (fs->f_flags & MNT_DONTBROWSE) == 0;
}



static void
thunar_macos_volume_monitor_refresh (GVolumeMonitor *gmonitor)
{
  ThunarMacosVolumeMonitor *monitor = THUNAR_MACOS_VOLUME_MONITOR (gmonitor);
  struct statfs            *fs;
  GHashTable               *current;
  GHashTableIter            iter;
  gpointer                  key, value;
  GList                    *removed = NULL, *added = NULL, *lp;
  gint                      n, i;

  current = g_hash_table_new (g_str_hash, g_str_equal);
  n = getmntinfo (&fs, MNT_NOWAIT);
  for (i = 0; i < n; ++i)
    if (mount_is_user_visible (&fs[i]))
      g_hash_table_add (current, fs[i].f_mntonname);

  /* gone */
  g_hash_table_iter_init (&iter, monitor->mounts);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (!g_hash_table_contains (current, key))
      {
        removed = g_list_prepend (removed, g_object_ref (value));
        g_hash_table_iter_remove (&iter);
      }

  /* new */
  g_hash_table_iter_init (&iter, current);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    if (!g_hash_table_contains (monitor->mounts, key))
      {
        ThunarMacosMount *mount = thunar_macos_mount_new (gmonitor, key);
        g_hash_table_insert (monitor->mounts, g_strdup (key), mount);
        added = g_list_prepend (added, g_object_ref (mount));
      }

  g_hash_table_destroy (current);

  for (lp = removed; lp != NULL; lp = lp->next)
    {
      g_signal_emit_by_name (lp->data, "unmounted");
      g_signal_emit_by_name (gmonitor, "mount-removed", lp->data);
    }
  for (lp = added; lp != NULL; lp = lp->next)
    g_signal_emit_by_name (gmonitor, "mount-added", lp->data);

  g_list_free_full (removed, g_object_unref);
  g_list_free_full (added, g_object_unref);
}



static gboolean
thunar_macos_volume_monitor_poll (gpointer user_data)
{
  thunar_macos_volume_monitor_refresh (G_VOLUME_MONITOR (user_data));
  return G_SOURCE_CONTINUE;
}



static gboolean
thunar_macos_volume_monitor_is_supported (void)
{
  return TRUE;
}



static GList *
thunar_macos_volume_monitor_get_mounts (GVolumeMonitor *gmonitor)
{
  ThunarMacosVolumeMonitor *monitor = THUNAR_MACOS_VOLUME_MONITOR (gmonitor);
  GList                    *list = g_hash_table_get_values (monitor->mounts);

  g_list_foreach (list, (GFunc) (void (*) (void)) g_object_ref, NULL);
  return list;
}



static GList *
thunar_macos_volume_monitor_get_empty_list (GVolumeMonitor *monitor)
{
  return NULL;
}



static GVolume *
thunar_macos_volume_monitor_get_volume_for_uuid (GVolumeMonitor *monitor,
                                                 const gchar    *uuid)
{
  return NULL;
}



static GMount *
thunar_macos_volume_monitor_get_mount_for_uuid (GVolumeMonitor *gmonitor,
                                                const gchar    *uuid)
{
  ThunarMacosVolumeMonitor *monitor = THUNAR_MACOS_VOLUME_MONITOR (gmonitor);
  GHashTableIter            iter;
  gpointer                  value;

  g_hash_table_iter_init (&iter, monitor->mounts);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    if (g_strcmp0 (THUNAR_MACOS_MOUNT (value)->uuid, uuid) == 0)
      return g_object_ref (value);

  return NULL;
}



static GMount *
thunar_macos_volume_monitor_get_mount_for_mount_path (const gchar  *mount_path,
                                                      GCancellable *cancellable)
{
  GMount *mount;

  if (the_monitor == NULL)
    return NULL;

  mount = g_hash_table_lookup (the_monitor->mounts, mount_path);
  return mount != NULL ? g_object_ref (mount) : NULL;
}



static void
thunar_macos_volume_monitor_finalize (GObject *object)
{
  ThunarMacosVolumeMonitor *monitor = THUNAR_MACOS_VOLUME_MONITOR (object);

  if (the_monitor == monitor)
    the_monitor = NULL;
  if (monitor->poll_id != 0)
    g_source_remove (monitor->poll_id);
  g_hash_table_destroy (monitor->mounts);

  G_OBJECT_CLASS (thunar_macos_volume_monitor_parent_class)->finalize (object);
}



static void
thunar_macos_volume_monitor_class_init (ThunarMacosVolumeMonitorClass *klass)
{
  GObjectClass              *gobject_class = G_OBJECT_CLASS (klass);
  GVolumeMonitorClass       *monitor_class = G_VOLUME_MONITOR_CLASS (klass);
  GNativeVolumeMonitorClass *native_class = G_NATIVE_VOLUME_MONITOR_CLASS (klass);

  gobject_class->finalize = thunar_macos_volume_monitor_finalize;

  monitor_class->is_supported = thunar_macos_volume_monitor_is_supported;
  monitor_class->get_mounts = thunar_macos_volume_monitor_get_mounts;
  monitor_class->get_volumes = thunar_macos_volume_monitor_get_empty_list;
  monitor_class->get_connected_drives = thunar_macos_volume_monitor_get_empty_list;
  monitor_class->get_volume_for_uuid = thunar_macos_volume_monitor_get_volume_for_uuid;
  monitor_class->get_mount_for_uuid = thunar_macos_volume_monitor_get_mount_for_uuid;

  native_class->get_mount_for_mount_path = thunar_macos_volume_monitor_get_mount_for_mount_path;
}



static void
thunar_macos_volume_monitor_init (ThunarMacosVolumeMonitor *monitor)
{
  monitor->mounts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  the_monitor = monitor;

  thunar_macos_volume_monitor_refresh (G_VOLUME_MONITOR (monitor));
  monitor->poll_id = g_timeout_add_seconds (POLL_INTERVAL_SECONDS, thunar_macos_volume_monitor_poll, monitor);
}



/**
 * thunar_macos_volume_monitor_register:
 *
 * Makes the macOS volume monitor GIO's native volume monitor. Must be
 * called before the first g_volume_monitor_get().
 **/
void
thunar_macos_volume_monitor_register (void)
{
  /* make sure GIO registered its extension points */
  g_vfs_get_default ();

  g_io_extension_point_implement (G_NATIVE_VOLUME_MONITOR_EXTENSION_POINT_NAME,
                                  THUNAR_TYPE_MACOS_VOLUME_MONITOR,
                                  "thunar-macos", 10);
}
