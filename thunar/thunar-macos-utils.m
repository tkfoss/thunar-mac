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

/* macOS helpers. Compiled as Objective-C (ARC) without _XOPEN_SOURCE so all
 * Darwin APIs (xattr, statfs, removefile, getmntinfo) are available. */

#import <Foundation/Foundation.h>

#include <errno.h>
#include <removefile.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "thunar/thunar-macos.h"

#define XATTR_ORIG_PATH     "org.xfce.thunar.trash.orig-path"
#define XATTR_DELETION_TIME "org.xfce.thunar.trash.deletion-time"



/* ---------------------------------------------------------------------- */
/* xattr helpers                                                          */
/* ---------------------------------------------------------------------- */

static gchar *
get_xattr_string (const gchar *path,
                  const gchar *name)
{
  ssize_t len = getxattr (path, name, NULL, 0, 0, XATTR_NOFOLLOW);
  gchar  *value;

  if (len <= 0 || len > 64 * 1024)
    return NULL;

  value = g_malloc0 (len + 1);
  if (getxattr (path, name, value, len, 0, XATTR_NOFOLLOW) != len)
    {
      g_free (value);
      return NULL;
    }

  return value;
}



static void
set_xattr_string (const gchar *path,
                  const gchar *name,
                  const gchar *value)
{
  setxattr (path, name, value, strlen (value), 0, XATTR_NOFOLLOW);
}



/* ---------------------------------------------------------------------- */
/* Trashing                                                               */
/* ---------------------------------------------------------------------- */

gboolean
thunar_macos_trash_file (GFile        *file,
                         GCancellable *cancellable,
                         GError      **error)
{
  gchar   *path;
  gboolean success = FALSE;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  /* non-local files are left to GIO */
  if (!g_file_is_native (file) || (path = g_file_get_path (file)) == NULL)
    return g_file_trash (file, cancellable, error);

  @autoreleasepool
    {
      NSURL   *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
      NSURL   *result = nil;
      NSError *ns_error = nil;

      if ([[NSFileManager defaultManager] trashItemAtURL:url resultingItemURL:&result error:&ns_error])
        {
          success = TRUE;
          if (result != nil)
            {
              gchar *when = g_strdup_printf ("%" G_GINT64_FORMAT, g_get_real_time () / G_USEC_PER_SEC);
              const char *trashed = result.path.fileSystemRepresentation;

              set_xattr_string (trashed, XATTR_ORIG_PATH, path);
              set_xattr_string (trashed, XATTR_DELETION_TIME, when);
              g_free (when);
            }
        }
      else
        {
          gint code = G_IO_ERROR_FAILED;

          if ([ns_error.domain isEqualToString:NSCocoaErrorDomain])
            {
              if (ns_error.code == NSFileNoSuchFileError || ns_error.code == NSFileReadNoSuchFileError)
                code = G_IO_ERROR_NOT_FOUND;
              else if (ns_error.code == NSFileWriteNoPermissionError || ns_error.code == NSFileReadNoPermissionError)
                code = G_IO_ERROR_PERMISSION_DENIED;
              else if (ns_error.code == NSFeatureUnsupportedError)
                code = G_IO_ERROR_NOT_SUPPORTED;
            }

          g_set_error (error, G_IO_ERROR, code, "%s", ns_error.localizedDescription.UTF8String);
        }
    }

  g_free (path);
  return success;
}



const gchar *
thunar_macos_home_trash_dir (void)
{
  static gchar *dir = NULL;

  if (g_once_init_enter_pointer (&dir))
    {
      const gchar *override = g_getenv ("THUNAR_MACOS_TRASH_DIR");
      gchar       *value;

      if (override != NULL && *override != '\0')
        value = g_strdup (override);
      else
        value = g_build_filename (g_get_home_dir (), ".Trash", NULL);

      g_once_init_leave_pointer (&dir, value);
    }

  return dir;
}



gchar **
thunar_macos_trash_dirs (void)
{
  GPtrArray     *dirs = g_ptr_array_new ();
  struct statfs *mounts = NULL;
  int            n_mounts;
  gchar         *uid;

  g_ptr_array_add (dirs, g_strdup (thunar_macos_home_trash_dir ()));

  /* when testing with an overridden home trash, ignore real volumes */
  if (g_getenv ("THUNAR_MACOS_TRASH_DIR") == NULL)
    {
      uid = g_strdup_printf ("%u", (guint) getuid ());
      n_mounts = getmntinfo (&mounts, MNT_NOWAIT);
      for (int n = 0; n < n_mounts; ++n)
        {
          const char *mnt = mounts[n].f_mntonname;
          gchar      *trash;

          if (!g_str_has_prefix (mnt, "/Volumes/"))
            continue;

          trash = g_build_filename (mnt, ".Trashes", uid, NULL);
          if (g_file_test (trash, G_FILE_TEST_IS_DIR))
            g_ptr_array_add (dirs, trash);
          else
            g_free (trash);
        }
      g_free (uid);
    }

  g_ptr_array_add (dirs, NULL);
  return (gchar **) g_ptr_array_free (dirs, FALSE);
}



/* ---------------------------------------------------------------------- */
/* Finder .DS_Store "Put Back" information                               */
/* ---------------------------------------------------------------------- */

/* The .DS_Store file is a B-tree stored in a "buddy allocator" file. Each
 * record is (filename, 4cc code, typed value). Finder stores the original
 * parent directory (relative to the volume root) as 'ptbL' and the original
 * name as 'ptbN' for every item it moves to the trash. */

typedef struct
{
  const guint8 *data;
  gsize         size;
  guint32      *offsets; /* block id -> offset (relative to data + 4) */
  guint32      *sizes;
  guint32       n_blocks;
  GHashTable   *ptbL;    /* name -> parent path */
  GHashTable   *ptbN;    /* name -> original name */
} DSStore;



static gboolean
ds_u32 (const DSStore *ds,
        gsize          pos,
        guint32       *out)
{
  if (pos + 4 > ds->size)
    return FALSE;
  *out = ((guint32) ds->data[pos] << 24) | ((guint32) ds->data[pos + 1] << 16)
         | ((guint32) ds->data[pos + 2] << 8) | (guint32) ds->data[pos + 3];
  return TRUE;
}



static gchar *
ds_utf16 (const DSStore *ds,
          gsize          pos,
          guint32        n_chars)
{
  gunichar2 *buf;
  gchar     *str;

  if (n_chars > 4096 || pos + (gsize) n_chars * 2 > ds->size)
    return NULL;

  buf = g_new (gunichar2, n_chars + 1);
  for (guint32 i = 0; i < n_chars; ++i)
    buf[i] = (gunichar2) ((ds->data[pos + 2 * i] << 8) | ds->data[pos + 2 * i + 1]);
  buf[n_chars] = 0;
  str = g_utf16_to_utf8 (buf, n_chars, NULL, NULL, NULL);
  g_free (buf);
  return str;
}



/* parses one record at *pos, advances *pos */
static gboolean
ds_record (DSStore *ds,
           gsize   *pos)
{
  guint32 name_len, code, type, len;
  gchar  *name;
  gsize   p = *pos;

  if (!ds_u32 (ds, p, &name_len) || name_len > 4096)
    return FALSE;
  p += 4;
  name = ds_utf16 (ds, p, name_len);
  p += (gsize) name_len * 2;
  if (!ds_u32 (ds, p, &code) || !ds_u32 (ds, p + 4, &type))
    {
      g_free (name);
      return FALSE;
    }
  p += 8;

  switch (type)
    {
    case 0x6C6F6E67: /* long */
    case 0x73686F72: /* shor */
    case 0x74797065: /* type */
      p += 4;
      break;
    case 0x626F6F6C: /* bool */
      p += 1;
      break;
    case 0x636F6D70: /* comp */
    case 0x64757463: /* dutc */
      p += 8;
      break;
    case 0x626C6F62: /* blob */
      if (!ds_u32 (ds, p, &len))
        goto fail;
      p += 4 + len;
      break;
    case 0x75737472: /* ustr */
      if (!ds_u32 (ds, p, &len))
        goto fail;
      if (name != NULL && (code == 0x7074624C /* ptbL */ || code == 0x7074624E /* ptbN */))
        {
          gchar *value = ds_utf16 (ds, p + 4, len);
          if (value != NULL)
            g_hash_table_replace (code == 0x7074624C ? ds->ptbL : ds->ptbN, g_strdup (name), value);
        }
      p += 4 + (gsize) len * 2;
      break;
    default:
      goto fail;
    }

  g_free (name);
  *pos = p;
  return p <= ds->size;

fail:
  g_free (name);
  return FALSE;
}



static gboolean
ds_node (DSStore *ds,
         guint32  block,
         guint    depth)
{
  guint32 next, count, child;
  gsize   pos;

  if (depth > 16 || block >= ds->n_blocks)
    return FALSE;

  pos = 4 + ds->offsets[block];
  if (!ds_u32 (ds, pos, &next) || !ds_u32 (ds, pos + 4, &count) || count > 100000)
    return FALSE;
  pos += 8;

  for (guint32 i = 0; i < count; ++i)
    {
      if (next != 0)
        {
          if (!ds_u32 (ds, pos, &child) || !ds_node (ds, child, depth + 1))
            return FALSE;
          pos += 4;
        }
      if (!ds_record (ds, &pos))
        return FALSE;
    }

  if (next != 0)
    return ds_node (ds, next, depth + 1);

  return TRUE;
}



static gboolean
ds_parse (DSStore *ds)
{
  guint32 magic, root_off, n_blocks, n_toc, master = G_MAXUINT32, root_node;
  gsize   pos;

  if (!ds_u32 (ds, 4, &magic) || magic != 0x42756431 /* Bud1 */ || !ds_u32 (ds, 8, &root_off))
    return FALSE;

  pos = 4 + root_off;
  if (!ds_u32 (ds, pos, &n_blocks) || n_blocks > 1000000)
    return FALSE;
  pos += 8;

  ds->n_blocks = n_blocks;
  ds->offsets = g_new0 (guint32, n_blocks);
  for (guint32 i = 0; i < n_blocks; ++i)
    {
      guint32 addr;
      if (!ds_u32 (ds, pos + 4 * i, &addr))
        return FALSE;
      ds->offsets[i] = addr & ~0x1fU;
    }
  /* block address table is padded to a multiple of 256 entries */
  pos += 4 * (((n_blocks + 255) / 256) * 256);

  /* table of contents: find "DSDB" */
  if (!ds_u32 (ds, pos, &n_toc) || n_toc > 1000)
    return FALSE;
  pos += 4;
  for (guint32 i = 0; i < n_toc; ++i)
    {
      guint32 value;
      guint8  len;

      if (pos >= ds->size)
        return FALSE;
      len = ds->data[pos++];
      if (pos + len + 4 > ds->size)
        return FALSE;
      ds_u32 (ds, pos + len, &value);
      if (len == 4 && memcmp (ds->data + pos, "DSDB", 4) == 0)
        master = value;
      pos += len + 4;
    }

  if (master >= n_blocks || !ds_u32 (ds, 4 + ds->offsets[master], &root_node))
    return FALSE;

  return ds_node (ds, root_node, 0);
}



typedef struct
{
  gint64      mtime;
  GHashTable *ptbL;
  GHashTable *ptbN;
} DSStoreCacheEntry;

static GMutex      ds_cache_lock;
static GHashTable *ds_cache = NULL; /* trash dir -> DSStoreCacheEntry */



static void
ds_cache_entry_free (gpointer data)
{
  DSStoreCacheEntry *entry = data;
  g_hash_table_unref (entry->ptbL);
  g_hash_table_unref (entry->ptbN);
  g_free (entry);
}



/* returns the "Put Back" path for @name in @trash_dir, or NULL */
static gchar *
ds_lookup_put_back (const gchar *trash_dir,
                    const gchar *name,
                    const gchar *volume_root)
{
  DSStoreCacheEntry *entry;
  struct stat        st;
  gchar             *ds_path = g_build_filename (trash_dir, ".DS_Store", NULL);
  gchar             *result = NULL;

  if (stat (ds_path, &st) != 0)
    {
      g_free (ds_path);
      return NULL;
    }

  g_mutex_lock (&ds_cache_lock);

  if (ds_cache == NULL)
    ds_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, ds_cache_entry_free);

  entry = g_hash_table_lookup (ds_cache, trash_dir);
  if (entry == NULL || entry->mtime != (gint64) st.st_mtime)
    {
      GMappedFile *mapped = g_mapped_file_new (ds_path, FALSE, NULL);
      DSStore      ds = { 0 };

      entry = g_new0 (DSStoreCacheEntry, 1);
      entry->mtime = st.st_mtime;
      entry->ptbL = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      entry->ptbN = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

      if (mapped != NULL)
        {
          ds.data = (const guint8 *) g_mapped_file_get_contents (mapped);
          ds.size = g_mapped_file_get_length (mapped);
          ds.ptbL = entry->ptbL;
          ds.ptbN = entry->ptbN;
          if (ds.data != NULL)
            ds_parse (&ds);
          g_free (ds.offsets);
          g_mapped_file_unref (mapped);
        }

      g_hash_table_replace (ds_cache, g_strdup (trash_dir), entry);
    }

  {
    const gchar *parent = g_hash_table_lookup (entry->ptbL, name);
    const gchar *orig_name = g_hash_table_lookup (entry->ptbN, name);

    if (parent != NULL)
      result = g_build_filename (volume_root, parent, orig_name != NULL ? orig_name : name, NULL);
  }

  g_mutex_unlock (&ds_cache_lock);
  g_free (ds_path);

  return result;
}



gchar *
thunar_macos_trash_get_orig_path (const gchar *trash_dir,
                                  const gchar *item_path)
{
  struct statfs sfs;
  gchar        *path;
  gchar        *name;
  const gchar  *volume_root = "/";

  /* 1. recorded by Thunar when trashing */
  path = get_xattr_string (item_path, XATTR_ORIG_PATH);
  if (path != NULL)
    return path;

  /* 2. recorded by Finder (Put Back) */
  if (statfs (trash_dir, &sfs) == 0)
    volume_root = sfs.f_mntonname;
  /* the boot volume's data partition is mounted at /System/Volumes/Data,
   * but Finder stores paths relative to / */
  if (g_str_has_prefix (volume_root, "/System/Volumes/"))
    volume_root = "/";

  name = g_path_get_basename (item_path);
  path = ds_lookup_put_back (trash_dir, name, volume_root);
  g_free (name);

  return path;
}



gint64
thunar_macos_trash_get_deletion_time (const gchar *item_path)
{
  struct stat st;
  gchar      *value = get_xattr_string (item_path, XATTR_DELETION_TIME);
  gint64      result = 0;

  if (value != NULL)
    {
      result = g_ascii_strtoll (value, NULL, 10);
      g_free (value);
    }

  /* moving into the trash updates the inode change time */
  if (result <= 0 && lstat (item_path, &st) == 0)
    result = st.st_ctime;

  return result;
}



void
thunar_macos_trash_clear_metadata (const gchar *path)
{
  removexattr (path, XATTR_ORIG_PATH, XATTR_NOFOLLOW);
  removexattr (path, XATTR_DELETION_TIME, XATTR_NOFOLLOW);
}



gboolean
thunar_macos_remove_recursive (const gchar *path,
                               GError     **error)
{
  removefile_state_t state = removefile_state_alloc ();
  int                result = removefile (path, state, REMOVEFILE_RECURSIVE);
  int                saved_errno = errno;

  removefile_state_free (state);

  if (result != 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                   "%s", g_strerror (saved_errno));
      return FALSE;
    }

  return TRUE;
}
