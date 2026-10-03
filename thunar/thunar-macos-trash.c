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

/*
 * A "trash://" GIO backend for macOS (GVfs is not available there).
 *
 * trash:///                 virtual root, merges all trash directories
 * trash:///<name>           top-level item in the home trash (~/.Trash/<name>)
 * trash:///\Volumes\X\.Trashes\501\<name>
 *                           top-level item in a volume trash ('/' encoded as '\')
 * trash:///<top>/<sub>/...  items inside trashed folders
 *
 * Top-level items get trash::orig-path and trash::deletion-date, like GVfs.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "thunar/thunar-macos.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <string.h>

#define TRASH_SCHEME "trash"



/* ---------------------------------------------------------------------- */
/* helpers                                                                */
/* ---------------------------------------------------------------------- */

static gboolean
is_ignored_name (const gchar *name)
{
  return g_strcmp0 (name, ".DS_Store") == 0 || g_strcmp0 (name, ".localized") == 0;
}



/* name of a top-level item as shown in trash:/// */
static gchar *
top_name_for (const gchar *trash_dir,
              const gchar *basename)
{
  gchar *path;

  if (g_strcmp0 (trash_dir, thunar_macos_home_trash_dir ()) == 0)
    return g_strdup (basename);

  path = g_build_filename (trash_dir, basename, NULL);
  g_strdelimit (path, "/", '\\');
  return path;
}



/* resolves a top-level name to (trash_dir, real path); FALSE if invalid */
static gboolean
resolve_top_name (const gchar *top,
                  gchar      **trash_dir_out,
                  gchar      **real_out)
{
  gchar *real;
  gchar *dir;

  if (top == NULL || *top == '\0' || strcmp (top, ".") == 0 || strcmp (top, "..") == 0)
    return FALSE;

  if (top[0] == '\\')
    {
      real = g_strdup (top);
      g_strdelimit (real, "\\", '/');
      dir = g_path_get_dirname (real);

      /* only allow items that really live in a volume trash */
      if (!g_str_has_prefix (real, "/Volumes/") || strstr (dir, "/.Trashes/") == NULL || strstr (real, "/../") != NULL)
        {
          g_free (real);
          g_free (dir);
          return FALSE;
        }
    }
  else
    {
      if (strchr (top, '/') != NULL)
        return FALSE;
      dir = g_strdup (thunar_macos_home_trash_dir ());
      real = g_build_filename (dir, top, NULL);
    }

  if (trash_dir_out != NULL)
    *trash_dir_out = dir;
  else
    g_free (dir);

  *real_out = real;
  return TRUE;
}



/* canonicalizes a relative path: drops empty and "." components, applies ".." */
static gchar *
canonicalize_rel (const gchar *rel)
{
  gchar    **parts = g_strsplit (rel, "/", -1);
  GPtrArray *out = g_ptr_array_new ();
  gchar     *result;

  for (guint i = 0; parts[i] != NULL; ++i)
    {
      if (*parts[i] == '\0' || strcmp (parts[i], ".") == 0)
        continue;
      if (strcmp (parts[i], "..") == 0)
        {
          if (out->len > 0)
            g_ptr_array_remove_index (out, out->len - 1);
          continue;
        }
      g_ptr_array_add (out, parts[i]);
    }
  g_ptr_array_add (out, NULL);

  result = g_strjoinv ("/", (gchar **) out->pdata);
  g_ptr_array_free (out, TRUE);
  g_strfreev (parts);

  return result;
}



/* ---------------------------------------------------------------------- */
/* ThunarMacosTrashFile                                                   */
/* ---------------------------------------------------------------------- */

#define THUNAR_TYPE_MACOS_TRASH_FILE (thunar_macos_trash_file_get_type ())
G_DECLARE_FINAL_TYPE (ThunarMacosTrashFile, thunar_macos_trash_file, THUNAR, MACOS_TRASH_FILE, GObject)

struct _ThunarMacosTrashFile
{
  GObject __parent__;
  gchar  *rel; /* "" for the root, otherwise "<top>[/<sub>...]" (unescaped) */
};

static void thunar_macos_trash_file_iface_init (GFileIface *iface);

G_DEFINE_TYPE_WITH_CODE (ThunarMacosTrashFile, thunar_macos_trash_file, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_FILE, thunar_macos_trash_file_iface_init))



static void
thunar_macos_trash_file_finalize (GObject *object)
{
  g_free (THUNAR_MACOS_TRASH_FILE (object)->rel);
  G_OBJECT_CLASS (thunar_macos_trash_file_parent_class)->finalize (object);
}



static void
thunar_macos_trash_file_class_init (ThunarMacosTrashFileClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = thunar_macos_trash_file_finalize;
}



static void
thunar_macos_trash_file_init (ThunarMacosTrashFile *file)
{
}



static GFile *
trash_file_new (const gchar *rel)
{
  ThunarMacosTrashFile *file = g_object_new (THUNAR_TYPE_MACOS_TRASH_FILE, NULL);
  file->rel = canonicalize_rel (rel != NULL ? rel : "");
  return G_FILE (file);
}



static gboolean
trash_file_is_root (ThunarMacosTrashFile *file)
{
  return *file->rel == '\0';
}



static gboolean
trash_file_is_top (ThunarMacosTrashFile *file)
{
  return *file->rel != '\0' && strchr (file->rel, '/') == NULL;
}



/* real local path of a non-root item (NULL for root or invalid names) */
static gchar *
trash_file_real_path (ThunarMacosTrashFile *file,
                      gchar               **trash_dir_out)
{
  const gchar *slash;
  gchar       *top;
  gchar       *real_top;
  gchar       *real;

  if (trash_file_is_root (file))
    return NULL;

  slash = strchr (file->rel, '/');
  top = slash != NULL ? g_strndup (file->rel, slash - file->rel) : g_strdup (file->rel);

  if (!resolve_top_name (top, trash_dir_out, &real_top))
    {
      g_free (top);
      return NULL;
    }
  g_free (top);

  if (slash == NULL)
    return real_top;

  real = g_build_filename (real_top, slash + 1, NULL);
  g_free (real_top);
  return real;
}



static GFile *
trash_file_real_file (ThunarMacosTrashFile *file,
                      GError              **error)
{
  gchar *path = trash_file_real_path (file, NULL);
  GFile *real;

  if (path == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      return NULL;
    }

  real = g_file_new_for_path (path);
  g_free (path);
  return real;
}



static GError *
trash_permission_error (const GError *cause)
{
  return g_error_new (G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                      _("Cannot open the Trash: %s\n\nGrant \"Full Disk Access\" to Thunar (or the terminal "
                        "it is started from) in System Settings > Privacy & Security."),
                      cause != NULL ? cause->message : g_strerror (EPERM));
}



/* fills trash-specific attributes on the info of a real item */
static void
patch_info (GFileInfo   *info,
            const gchar *name,
            const gchar *trash_dir,
            const gchar *real_path,
            gboolean     is_top)
{
  g_file_info_set_name (info, name);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, is_top);

  if (is_top)
    {
      gchar     *orig = thunar_macos_trash_get_orig_path (trash_dir, real_path);
      gint64     when = thunar_macos_trash_get_deletion_time (real_path);
      GDateTime *dt = g_date_time_new_from_unix_local (when);

      if (orig != NULL)
        g_file_info_set_attribute_byte_string (info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH, orig);

      if (dt != NULL)
        {
          gchar *date = g_date_time_format (dt, "%Y-%m-%dT%H:%M:%S");
          g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_TRASH_DELETION_DATE, date);
          g_free (date);
          g_date_time_unref (dt);
        }

      g_free (orig);
    }
}



static guint32
trash_count_items (void)
{
  gchar **dirs = thunar_macos_trash_dirs ();
  guint32 count = 0;

  for (guint i = 0; dirs[i] != NULL; ++i)
    {
      GDir        *dir = g_dir_open (dirs[i], 0, NULL);
      const gchar *name;

      if (dir == NULL)
        continue;
      while ((name = g_dir_read_name (dir)) != NULL)
        if (!is_ignored_name (name))
          count++;
      g_dir_close (dir);
    }

  g_strfreev (dirs);
  return count;
}



static GFileInfo *
trash_root_info (void)
{
  GFileInfo *info = g_file_info_new ();
  guint32    count = trash_count_items ();
  GIcon     *icon;
  gchar     *content_type;
  GFile     *home_trash;
  GFileInfo *dir_info;

  g_file_info_set_name (info, "/");
  g_file_info_set_display_name (info, _("Trash"));
  g_file_info_set_edit_name (info, _("Trash"));
  g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);

  content_type = g_content_type_from_mime_type ("inode/directory");
  g_file_info_set_content_type (info, content_type != NULL ? content_type : "inode/directory");
  g_free (content_type);

  icon = g_themed_icon_new (count > 0 ? "user-trash-full" : "user-trash");
  g_file_info_set_icon (info, icon);
  g_object_unref (icon);
  icon = g_themed_icon_new (count > 0 ? "user-trash-full-symbolic" : "user-trash-symbolic");
  g_file_info_set_symbolic_icon (info, icon);
  g_object_unref (icon);

  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT, count);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);

  /* modification time of the home trash, so views can refresh */
  home_trash = g_file_new_for_path (thunar_macos_home_trash_dir ());
  dir_info = g_file_query_info (home_trash, G_FILE_ATTRIBUTE_TIME_MODIFIED, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (dir_info != NULL)
    {
      g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                        g_file_info_get_attribute_uint64 (dir_info, G_FILE_ATTRIBUTE_TIME_MODIFIED));
      g_object_unref (dir_info);
    }
  g_object_unref (home_trash);

  return info;
}



/* ---------------------------------------------------------------------- */
/* ThunarMacosTrashEnumerator                                             */
/* ---------------------------------------------------------------------- */

#define THUNAR_TYPE_MACOS_TRASH_ENUMERATOR (thunar_macos_trash_enumerator_get_type ())
G_DECLARE_FINAL_TYPE (ThunarMacosTrashEnumerator, thunar_macos_trash_enumerator, THUNAR, MACOS_TRASH_ENUMERATOR, GFileEnumerator)

struct _ThunarMacosTrashEnumerator
{
  GFileEnumerator  __parent__;
  GList           *infos; /* root: pre-collected infos */
  GFileEnumerator *inner; /* sub directories: wrapped real enumerator */
};

G_DEFINE_TYPE (ThunarMacosTrashEnumerator, thunar_macos_trash_enumerator, G_TYPE_FILE_ENUMERATOR)



static GFileInfo *
thunar_macos_trash_enumerator_next_file (GFileEnumerator *enumerator,
                                         GCancellable    *cancellable,
                                         GError         **error)
{
  ThunarMacosTrashEnumerator *self = THUNAR_MACOS_TRASH_ENUMERATOR (enumerator);
  GFileInfo                  *info;

  if (self->inner != NULL)
    {
      /* children of trashed folders keep their names; just adjust permissions */
      info = g_file_enumerator_next_file (self->inner, cancellable, error);
      if (info != NULL)
        {
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
        }
      return info;
    }

  if (self->infos == NULL)
    return NULL;

  info = self->infos->data;
  self->infos = g_list_delete_link (self->infos, self->infos);
  return info;
}



static gboolean
thunar_macos_trash_enumerator_close (GFileEnumerator *enumerator,
                                     GCancellable    *cancellable,
                                     GError         **error)
{
  ThunarMacosTrashEnumerator *self = THUNAR_MACOS_TRASH_ENUMERATOR (enumerator);

  if (self->inner != NULL)
    return g_file_enumerator_close (self->inner, cancellable, error);

  return TRUE;
}



static void
thunar_macos_trash_enumerator_finalize (GObject *object)
{
  ThunarMacosTrashEnumerator *self = THUNAR_MACOS_TRASH_ENUMERATOR (object);

  g_list_free_full (self->infos, g_object_unref);
  g_clear_object (&self->inner);

  G_OBJECT_CLASS (thunar_macos_trash_enumerator_parent_class)->finalize (object);
}



static void
thunar_macos_trash_enumerator_class_init (ThunarMacosTrashEnumeratorClass *klass)
{
  GFileEnumeratorClass *enum_class = G_FILE_ENUMERATOR_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = thunar_macos_trash_enumerator_finalize;
  enum_class->next_file = thunar_macos_trash_enumerator_next_file;
  enum_class->close_fn = thunar_macos_trash_enumerator_close;
}



static void
thunar_macos_trash_enumerator_init (ThunarMacosTrashEnumerator *self)
{
}



/* ---------------------------------------------------------------------- */
/* ThunarMacosTrashMonitor                                                */
/* ---------------------------------------------------------------------- */

#define THUNAR_TYPE_MACOS_TRASH_MONITOR (thunar_macos_trash_monitor_get_type ())
G_DECLARE_FINAL_TYPE (ThunarMacosTrashMonitor, thunar_macos_trash_monitor, THUNAR, MACOS_TRASH_MONITOR, GFileMonitor)

struct _ThunarMacosTrashMonitor
{
  GFileMonitor __parent__;

  ThunarMacosTrashFile *file;   /* the monitored trash location */
  gboolean              is_dir; /* directory monitor (map children) vs. file monitor */
  GList                *inner;  /* real GFileMonitors */

  /* fallback polling when the trash can't be watched (no Full Disk Access) */
  guint  poll_id;
  gint64 poll_stamp;
};

G_DEFINE_TYPE (ThunarMacosTrashMonitor, thunar_macos_trash_monitor, G_TYPE_FILE_MONITOR)



static GFile *
monitor_map_file (ThunarMacosTrashMonitor *self,
                  GFile                   *inner_dir,
                  GFile                   *real)
{
  gchar *basename;
  gchar *rel;
  GFile *mapped;

  if (real == NULL)
    return NULL;

  if (!self->is_dir || g_file_equal (real, inner_dir))
    return g_object_ref (G_FILE (self->file));

  basename = g_file_get_basename (real);
  if (basename == NULL || is_ignored_name (basename))
    {
      g_free (basename);
      return NULL;
    }

  if (trash_file_is_root (self->file))
    {
      gchar *dir_path = g_file_get_path (inner_dir);
      rel = top_name_for (dir_path, basename);
      g_free (dir_path);
    }
  else
    rel = g_build_filename (self->file->rel, basename, NULL);

  mapped = trash_file_new (rel);
  g_free (rel);
  g_free (basename);
  return mapped;
}



static void
monitor_inner_changed (GFileMonitor            *inner,
                       GFile                   *file,
                       GFile                   *other_file,
                       GFileMonitorEvent        event,
                       ThunarMacosTrashMonitor *self)
{
  GFile *inner_dir = g_object_get_data (G_OBJECT (inner), "thunar-trash-dir");
  GFile *mapped;
  GFile *mapped_other;

  if (g_file_monitor_is_cancelled (G_FILE_MONITOR (self)))
    return;

  /* the root's file monitor only reports that the trash changed (item count, icon) */
  if (!self->is_dir && trash_file_is_root (self->file))
    {
      if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
        g_file_monitor_emit_event (G_FILE_MONITOR (self), G_FILE (self->file), NULL,
                                   G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED);
      return;
    }

  mapped = monitor_map_file (self, inner_dir, file);
  if (mapped == NULL)
    return;

  mapped_other = monitor_map_file (self, inner_dir, other_file);
  g_file_monitor_emit_event (G_FILE_MONITOR (self), mapped, mapped_other, event);

  g_object_unref (mapped);
  if (mapped_other != NULL)
    g_object_unref (mapped_other);
}



static gint64
monitor_poll_stamp (void)
{
  gchar **dirs = thunar_macos_trash_dirs ();
  gint64  stamp = 0;

  for (guint i = 0; dirs[i] != NULL; ++i)
    {
      GStatBuf st;
      if (g_stat (dirs[i], &st) == 0)
        stamp = stamp * 31 + st.st_mtime + st.st_nlink + i;
    }

  g_strfreev (dirs);
  return stamp;
}



static gboolean
monitor_poll (gpointer user_data)
{
  ThunarMacosTrashMonitor *self = user_data;
  gint64                   stamp = monitor_poll_stamp ();

  if (stamp != self->poll_stamp)
    {
      self->poll_stamp = stamp;
      g_file_monitor_emit_event (G_FILE_MONITOR (self), G_FILE (self->file), NULL,
                                 G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED);
    }

  return G_SOURCE_CONTINUE;
}



static void
monitor_add_inner (ThunarMacosTrashMonitor *self,
                   const gchar             *real_path,
                   gboolean                 as_dir)
{
  GFile        *real = g_file_new_for_path (real_path);
  GFileMonitor *inner;

  inner = as_dir ? g_file_monitor_directory (real, G_FILE_MONITOR_NONE, NULL, NULL)
                 : g_file_monitor_file (real, G_FILE_MONITOR_NONE, NULL, NULL);
  if (inner != NULL)
    {
      g_object_set_data_full (G_OBJECT (inner), "thunar-trash-dir", g_object_ref (real), g_object_unref);
      g_signal_connect (inner, "changed", G_CALLBACK (monitor_inner_changed), self);
      self->inner = g_list_prepend (self->inner, inner);
    }

  g_object_unref (real);
}



static GFileMonitor *
trash_monitor_new (ThunarMacosTrashFile *file,
                   gboolean              is_dir)
{
  ThunarMacosTrashMonitor *self = g_object_new (THUNAR_TYPE_MACOS_TRASH_MONITOR, NULL);

  self->file = g_object_ref (file);
  self->is_dir = is_dir;

  if (trash_file_is_root (file))
    {
      gchar **dirs = thunar_macos_trash_dirs ();
      for (guint i = 0; dirs[i] != NULL; ++i)
        monitor_add_inner (self, dirs[i], TRUE);
      g_strfreev (dirs);

      /* the root's item count must stay correct even if the trash can't be watched */
      if (!is_dir)
        {
          self->poll_stamp = monitor_poll_stamp ();
          self->poll_id = g_timeout_add_seconds (2, monitor_poll, self);
        }
    }
  else
    {
      gchar *real = trash_file_real_path (file, NULL);
      if (real != NULL)
        monitor_add_inner (self, real, is_dir);
      g_free (real);
    }

  return G_FILE_MONITOR (self);
}



static gboolean
thunar_macos_trash_monitor_cancel (GFileMonitor *monitor)
{
  ThunarMacosTrashMonitor *self = THUNAR_MACOS_TRASH_MONITOR (monitor);

  for (GList *lp = self->inner; lp != NULL; lp = lp->next)
    {
      g_signal_handlers_disconnect_by_data (lp->data, self);
      g_file_monitor_cancel (lp->data);
    }

  if (self->poll_id != 0)
    {
      g_source_remove (self->poll_id);
      self->poll_id = 0;
    }

  return TRUE;
}



static void
thunar_macos_trash_monitor_finalize (GObject *object)
{
  ThunarMacosTrashMonitor *self = THUNAR_MACOS_TRASH_MONITOR (object);

  thunar_macos_trash_monitor_cancel (G_FILE_MONITOR (self));
  g_list_free_full (self->inner, g_object_unref);
  g_clear_object (&self->file);

  G_OBJECT_CLASS (thunar_macos_trash_monitor_parent_class)->finalize (object);
}



static void
thunar_macos_trash_monitor_class_init (ThunarMacosTrashMonitorClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = thunar_macos_trash_monitor_finalize;
  G_FILE_MONITOR_CLASS (klass)->cancel = thunar_macos_trash_monitor_cancel;
}



static void
thunar_macos_trash_monitor_init (ThunarMacosTrashMonitor *self)
{
}



/* ---------------------------------------------------------------------- */
/* GFile interface                                                        */
/* ---------------------------------------------------------------------- */

static GFile *
tf_dup (GFile *file)
{
  return trash_file_new (THUNAR_MACOS_TRASH_FILE (file)->rel);
}



static guint
tf_hash (GFile *file)
{
  return g_str_hash (THUNAR_MACOS_TRASH_FILE (file)->rel);
}



static gboolean
tf_equal (GFile *file1,
          GFile *file2)
{
  return THUNAR_IS_MACOS_TRASH_FILE (file2)
         && g_strcmp0 (THUNAR_MACOS_TRASH_FILE (file1)->rel, THUNAR_MACOS_TRASH_FILE (file2)->rel) == 0;
}



static gboolean
tf_is_native (GFile *file)
{
  return FALSE;
}



static gboolean
tf_has_uri_scheme (GFile      *file,
                   const char *uri_scheme)
{
  return g_ascii_strcasecmp (uri_scheme, TRASH_SCHEME) == 0;
}



static char *
tf_get_uri_scheme (GFile *file)
{
  return g_strdup (TRASH_SCHEME);
}



static char *
tf_get_basename (GFile *file)
{
  ThunarMacosTrashFile *self = THUNAR_MACOS_TRASH_FILE (file);
  const gchar          *slash;

  if (trash_file_is_root (self))
    return g_strdup ("/");

  slash = strrchr (self->rel, '/');
  return g_strdup (slash != NULL ? slash + 1 : self->rel);
}



static char *
tf_get_path (GFile *file)
{
  /* expose the real location so applications can open trashed files */
  return trash_file_real_path (THUNAR_MACOS_TRASH_FILE (file), NULL);
}



static char *
tf_get_uri (GFile *file)
{
  gchar *escaped = g_uri_escape_string (THUNAR_MACOS_TRASH_FILE (file)->rel,
                                        G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, FALSE);
  gchar *uri = g_strconcat (TRASH_SCHEME ":///", escaped, NULL);
  g_free (escaped);
  return uri;
}



static char *
tf_get_parse_name (GFile *file)
{
  return tf_get_uri (file);
}



static GFile *
tf_get_parent (GFile *file)
{
  ThunarMacosTrashFile *self = THUNAR_MACOS_TRASH_FILE (file);
  const gchar          *slash;
  gchar                *parent;
  GFile                *result;

  if (trash_file_is_root (self))
    return NULL;

  slash = strrchr (self->rel, '/');
  parent = slash != NULL ? g_strndup (self->rel, slash - self->rel) : g_strdup ("");
  result = trash_file_new (parent);
  g_free (parent);
  return result;
}



static gboolean
tf_prefix_matches (GFile *prefix,
                   GFile *file)
{
  const gchar *p, *f;
  gsize        len;

  if (!THUNAR_IS_MACOS_TRASH_FILE (prefix) || !THUNAR_IS_MACOS_TRASH_FILE (file))
    return FALSE;

  p = THUNAR_MACOS_TRASH_FILE (prefix)->rel;
  f = THUNAR_MACOS_TRASH_FILE (file)->rel;

  if (*p == '\0')
    return *f != '\0';

  len = strlen (p);
  return strncmp (p, f, len) == 0 && f[len] == '/';
}



static char *
tf_get_relative_path (GFile *parent,
                      GFile *descendant)
{
  const gchar *p;

  if (!tf_prefix_matches (parent, descendant))
    return NULL;

  p = THUNAR_MACOS_TRASH_FILE (parent)->rel;
  return g_strdup (THUNAR_MACOS_TRASH_FILE (descendant)->rel + (*p == '\0' ? 0 : strlen (p) + 1));
}



static GFile *
tf_resolve_relative_path (GFile      *file,
                          const char *relative_path)
{
  gchar *joined;
  GFile *result;

  if (relative_path[0] == '/')
    return trash_file_new (relative_path);

  joined = g_strconcat (THUNAR_MACOS_TRASH_FILE (file)->rel, "/", relative_path, NULL);
  result = trash_file_new (joined);
  g_free (joined);
  return result;
}



static GFile *
tf_get_child_for_display_name (GFile      *file,
                               const char *display_name,
                               GError    **error)
{
  return tf_resolve_relative_path (file, display_name);
}



static GFileEnumerator *
tf_enumerate_children (GFile              *file,
                       const char         *attributes,
                       GFileQueryInfoFlags flags,
                       GCancellable       *cancellable,
                       GError            **error)
{
  ThunarMacosTrashFile       *self = THUNAR_MACOS_TRASH_FILE (file);
  ThunarMacosTrashEnumerator *enumerator;
  gchar                      *attrs;

  /* we always need the name to map children */
  attrs = g_strconcat (G_FILE_ATTRIBUTE_STANDARD_NAME ",", attributes, NULL);

  enumerator = g_object_new (THUNAR_TYPE_MACOS_TRASH_ENUMERATOR, "container", file, NULL);

  if (trash_file_is_root (self))
    {
      gchar **dirs = thunar_macos_trash_dirs ();

      for (guint i = 0; dirs[i] != NULL; ++i)
        {
          GFile           *dir = g_file_new_for_path (dirs[i]);
          GError          *err = NULL;
          GFileEnumerator *inner = g_file_enumerate_children (dir, attrs, flags, cancellable, &err);
          GFileInfo       *info;

          if (inner == NULL)
            {
              /* the home trash must be readable; volume trashes are optional */
              if (i == 0)
                {
                  if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
                    {
                      g_propagate_error (error, trash_permission_error (err));
                      g_clear_error (&err);
                    }
                  else
                    g_propagate_error (error, err);

                  g_object_unref (dir);
                  g_strfreev (dirs);
                  g_free (attrs);
                  g_object_unref (enumerator);
                  return NULL;
                }
              g_clear_error (&err);
              g_object_unref (dir);
              continue;
            }

          while ((info = g_file_enumerator_next_file (inner, cancellable, NULL)) != NULL)
            {
              const gchar *basename = g_file_info_get_name (info);
              gchar       *real_path;
              gchar       *top;

              if (is_ignored_name (basename))
                {
                  g_object_unref (info);
                  continue;
                }

              real_path = g_build_filename (dirs[i], basename, NULL);
              top = top_name_for (dirs[i], basename);
              patch_info (info, top, dirs[i], real_path, TRUE);
              enumerator->infos = g_list_prepend (enumerator->infos, info);
              g_free (real_path);
              g_free (top);
            }

          g_file_enumerator_close (inner, NULL, NULL);
          g_object_unref (inner);
          g_object_unref (dir);
        }

      enumerator->infos = g_list_reverse (enumerator->infos);
      g_strfreev (dirs);
    }
  else
    {
      GFile *real = trash_file_real_file (self, error);

      if (real == NULL)
        {
          g_free (attrs);
          g_object_unref (enumerator);
          return NULL;
        }

      enumerator->inner = g_file_enumerate_children (real, attrs, flags, cancellable, error);
      g_object_unref (real);

      if (enumerator->inner == NULL)
        {
          g_free (attrs);
          g_object_unref (enumerator);
          return NULL;
        }
    }

  g_free (attrs);
  return G_FILE_ENUMERATOR (enumerator);
}



static GFileInfo *
tf_query_info (GFile              *file,
               const char         *attributes,
               GFileQueryInfoFlags flags,
               GCancellable       *cancellable,
               GError            **error)
{
  ThunarMacosTrashFile *self = THUNAR_MACOS_TRASH_FILE (file);
  GFileInfo            *info;
  gchar                *trash_dir = NULL;
  gchar                *real_path;
  gchar                *basename;
  GFile                *real;

  if (trash_file_is_root (self))
    return trash_root_info ();

  real_path = trash_file_real_path (self, &trash_dir);
  if (real_path == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("No such file or directory"));
      return NULL;
    }

  real = g_file_new_for_path (real_path);
  info = g_file_query_info (real, attributes, flags, cancellable, error);
  g_object_unref (real);

  if (info != NULL)
    {
      basename = tf_get_basename (file);
      patch_info (info, basename, trash_dir, real_path, trash_file_is_top (self));
      g_free (basename);
    }

  g_free (real_path);
  g_free (trash_dir);
  return info;
}



static GFileInfo *
tf_query_filesystem_info (GFile        *file,
                          const char   *attributes,
                          GCancellable *cancellable,
                          GError      **error)
{
  gchar     *real_path = trash_file_real_path (THUNAR_MACOS_TRASH_FILE (file), NULL);
  GFile     *real = g_file_new_for_path (real_path != NULL ? real_path : g_get_home_dir ());
  GFileInfo *info = g_file_query_filesystem_info (real, attributes, cancellable, error);

  g_object_unref (real);
  g_free (real_path);
  return info;
}



static GFileInputStream *
tf_read (GFile        *file,
         GCancellable *cancellable,
         GError      **error)
{
  GFile            *real = trash_file_real_file (THUNAR_MACOS_TRASH_FILE (file), error);
  GFileInputStream *stream;

  if (real == NULL)
    return NULL;

  stream = g_file_read (real, cancellable, error);
  g_object_unref (real);
  return stream;
}



static gboolean
tf_delete (GFile        *file,
           GCancellable *cancellable,
           GError      **error)
{
  ThunarMacosTrashFile *self = THUNAR_MACOS_TRASH_FILE (file);
  gchar                *real_path;
  gboolean              success;

  /* like GVfs: only top-level items can be deleted (recursively) */
  if (!trash_file_is_top (self))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           _("Items inside trashed folders cannot be deleted individually"));
      return FALSE;
    }

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  real_path = trash_file_real_path (self, NULL);
  success = real_path != NULL && thunar_macos_remove_recursive (real_path, error);
  g_free (real_path);

  return success;
}



static gboolean
tf_move (GFile                 *source,
         GFile                 *destination,
         GFileCopyFlags         flags,
         GCancellable          *cancellable,
         GFileProgressCallback  progress_callback,
         gpointer               progress_callback_data,
         GError               **error)
{
  GFile   *real;
  gboolean success;
  gboolean top;

  /* only moving out of the trash (restore) is supported */
  if (!THUNAR_IS_MACOS_TRASH_FILE (source) || THUNAR_IS_MACOS_TRASH_FILE (destination)
      || !g_file_is_native (destination))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      return FALSE;
    }

  real = trash_file_real_file (THUNAR_MACOS_TRASH_FILE (source), error);
  if (real == NULL)
    return FALSE;

  top = trash_file_is_top (THUNAR_MACOS_TRASH_FILE (source));
  success = g_file_move (real, destination, flags, cancellable, progress_callback, progress_callback_data, error);
  g_object_unref (real);

  if (success && top)
    {
      gchar *path = g_file_get_path (destination);
      if (path != NULL)
        thunar_macos_trash_clear_metadata (path);
      g_free (path);
    }

  return success;
}



static gboolean
tf_copy (GFile                 *source,
         GFile                 *destination,
         GFileCopyFlags         flags,
         GCancellable          *cancellable,
         GFileProgressCallback  progress_callback,
         gpointer               progress_callback_data,
         GError               **error)
{
  GFile   *real;
  gboolean success;

  if (!THUNAR_IS_MACOS_TRASH_FILE (source) || THUNAR_IS_MACOS_TRASH_FILE (destination))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      return FALSE;
    }

  real = trash_file_real_file (THUNAR_MACOS_TRASH_FILE (source), error);
  if (real == NULL)
    return FALSE;

  success = g_file_copy (real, destination, flags, cancellable, progress_callback, progress_callback_data, error);
  g_object_unref (real);

  return success;
}



static GFileMonitor *
tf_monitor_dir (GFile            *file,
                GFileMonitorFlags flags,
                GCancellable     *cancellable,
                GError          **error)
{
  return trash_monitor_new (THUNAR_MACOS_TRASH_FILE (file), TRUE);
}



static GFileMonitor *
tf_monitor_file (GFile            *file,
                 GFileMonitorFlags flags,
                 GCancellable     *cancellable,
                 GError          **error)
{
  return trash_monitor_new (THUNAR_MACOS_TRASH_FILE (file), FALSE);
}



static void
thunar_macos_trash_file_iface_init (GFileIface *iface)
{
  iface->dup = tf_dup;
  iface->hash = tf_hash;
  iface->equal = tf_equal;
  iface->is_native = tf_is_native;
  iface->has_uri_scheme = tf_has_uri_scheme;
  iface->get_uri_scheme = tf_get_uri_scheme;
  iface->get_basename = tf_get_basename;
  iface->get_path = tf_get_path;
  iface->get_uri = tf_get_uri;
  iface->get_parse_name = tf_get_parse_name;
  iface->get_parent = tf_get_parent;
  iface->prefix_matches = tf_prefix_matches;
  iface->get_relative_path = tf_get_relative_path;
  iface->resolve_relative_path = tf_resolve_relative_path;
  iface->get_child_for_display_name = tf_get_child_for_display_name;
  iface->enumerate_children = tf_enumerate_children;
  iface->query_info = tf_query_info;
  iface->query_filesystem_info = tf_query_filesystem_info;
  iface->read_fn = tf_read;
  iface->delete_file = tf_delete;
  iface->move = tf_move;
  iface->copy = tf_copy;
  iface->monitor_dir = tf_monitor_dir;
  iface->monitor_file = tf_monitor_file;
}



/* ---------------------------------------------------------------------- */
/* registration                                                           */
/* ---------------------------------------------------------------------- */

static GFile *
trash_lookup (GVfs       *vfs,
              const char *identifier,
              gpointer    user_data)
{
  const gchar *p = identifier;
  gchar       *rel;
  GFile       *file;

  /* accepts trash:, trash:/x, trash:///x, trash://x */
  if (g_ascii_strncasecmp (p, TRASH_SCHEME ":", strlen (TRASH_SCHEME ":")) != 0)
    return NULL;
  p += strlen (TRASH_SCHEME ":");
  while (*p == '/')
    ++p;

  rel = g_uri_unescape_string (p, NULL);
  if (rel == NULL)
    rel = g_strdup (p);

  file = trash_file_new (rel);
  g_free (rel);
  return file;
}



void
thunar_macos_trash_vfs_register (void)
{
  GVfs *vfs = g_vfs_get_default ();

  if (!g_vfs_register_uri_scheme (vfs, TRASH_SCHEME,
                                  trash_lookup, NULL, NULL,
                                  trash_lookup, NULL, NULL))
    g_warning ("Failed to register the trash:// URI scheme");
}
