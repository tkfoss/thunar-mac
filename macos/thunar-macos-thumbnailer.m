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
 * thunar-macos-thumbnailer: a minimal implementation of the freedesktop
 * thumbnail D-Bus specification (org.freedesktop.thumbnails.Thumbnailer1 and
 * .Cache1, as provided by tumbler on Linux) backed by Quick Look.
 *
 * Thumbnails are written to $XDG_CACHE_HOME/thumbnails/<flavor>/<md5(uri)>.png
 * with the Thumb::URI / Thumb::MTime keys, failures are remembered in
 * $XDG_CACHE_HOME/thumbnails/fail/thunar-macos/ so unsupported files are not
 * retried until they change.
 */

#import <Foundation/Foundation.h>
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>
#import <CoreGraphics/CoreGraphics.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "thunar-macos-thumbnailer-dbus.h"
#include "thunar-macos-thumbnail-cache-dbus.h"

#define THUMBNAILER_NAME "org.freedesktop.thumbnails.Thumbnailer1"
#define THUMBNAILER_PATH "/org/freedesktop/thumbnails/Thumbnailer1"
#define CACHE_NAME       "org.freedesktop.thumbnails.Cache1"
#define CACHE_PATH       "/org/freedesktop/thumbnails/Cache1"
#define FAIL_SUBDIR      "thunar-macos"
#define IDLE_TIMEOUT     (15 * 60)
#define MAX_PARALLEL     4

static const struct
{
  const gchar *flavor;
  gint         size;
} flavors[] = {
  { "normal", 128 },
  { "large", 256 },
  { "x-large", 512 },
  { "xx-large", 1024 },
};



typedef struct
{
  guint       handle;
  gchar      *flavor;
  gint        size;
  guint       remaining;
  gboolean    started;
  gboolean    cancelled;
  GPtrArray  *ready;  /* uris to report in the next Ready signal */
  GPtrArray  *failed; /* uris to report in the next Error signal */
  guint       flush_id;
} Request;



static GMainLoop                    *main_loop = NULL;
static TmtThumbnailerDBus           *thumbnailer = NULL;
static GHashTable                   *requests = NULL; /* handle -> Request */
static guint                         last_handle = 0;
static guint                         idle_id = 0;
static NSOperationQueue             *work_queue = nil;



/* ---------------------------------------------------------------------- */
/* paths                                                                  */
/* ---------------------------------------------------------------------- */

static gint
flavor_size (const gchar *flavor)
{
  for (guint i = 0; i < G_N_ELEMENTS (flavors); ++i)
    if (g_strcmp0 (flavors[i].flavor, flavor) == 0)
      return flavors[i].size;
  return 0;
}



static gchar *
uri_md5 (const gchar *uri)
{
  return g_compute_checksum_for_string (G_CHECKSUM_MD5, uri, -1);
}



static gchar *
thumbnail_path (const gchar *uri,
                const gchar *flavor)
{
  gchar *md5 = uri_md5 (uri);
  gchar *name = g_strconcat (md5, ".png", NULL);
  gchar *path = g_build_filename (g_get_user_cache_dir (), "thumbnails", flavor, name, NULL);
  g_free (md5);
  g_free (name);
  return path;
}



static gchar *
fail_path (const gchar *uri)
{
  gchar *md5 = uri_md5 (uri);
  gchar *name = g_strconcat (md5, ".png", NULL);
  gchar *path = g_build_filename (g_get_user_cache_dir (), "thumbnails", "fail", FAIL_SUBDIR, name, NULL);
  g_free (md5);
  g_free (name);
  return path;
}



/* ---------------------------------------------------------------------- */
/* thumbnail files                                                        */
/* ---------------------------------------------------------------------- */

static gboolean
thumbnail_is_valid (const gchar *path,
                    const gchar *uri,
                    gint64       mtime)
{
  GdkPixbuf   *pixbuf;
  const gchar *value;
  gboolean     valid = FALSE;
  GStatBuf     st;

  if (g_stat (path, &st) != 0 || st.st_mtime < mtime)
    return FALSE;

  pixbuf = gdk_pixbuf_new_from_file (path, NULL);
  if (pixbuf == NULL)
    return FALSE;

  value = gdk_pixbuf_get_option (pixbuf, "tEXt::Thumb::MTime");
  if (value != NULL && g_ascii_strtoll (value, NULL, 10) == mtime)
    {
      value = gdk_pixbuf_get_option (pixbuf, "tEXt::Thumb::URI");
      valid = (value == NULL || g_strcmp0 (value, uri) == 0);
    }

  g_object_unref (pixbuf);
  return valid;
}



static gboolean
fail_is_valid (const gchar *uri,
               gint64       mtime)
{
  gchar   *path = fail_path (uri);
  GStatBuf st;
  gboolean valid = (g_stat (path, &st) == 0 && st.st_mtime >= mtime);
  g_free (path);
  return valid;
}



static void
fail_record (const gchar *uri)
{
  gchar *path = fail_path (uri);
  gchar *dir = g_path_get_dirname (path);

  g_mkdir_with_parents (dir, 0700);
  g_file_set_contents (path, "", 0, NULL);

  g_free (dir);
  g_free (path);
}



/* writes @image as a spec-compliant PNG thumbnail (atomically) */
static gboolean
thumbnail_save (CGImageRef   image,
                const gchar *path,
                const gchar *uri,
                gint64       mtime,
                gint64       size)
{
  gsize      width = CGImageGetWidth (image);
  gsize      height = CGImageGetHeight (image);
  gsize      stride = width * 4;
  guchar    *pixels;
  CGContextRef ctx;
  CGColorSpaceRef cs;
  GdkPixbuf *pixbuf;
  gchar     *dir, *tmp, *mtime_str, *size_str;
  gboolean   ok;

  if (width == 0 || height == 0)
    return FALSE;

  pixels = g_malloc0 (stride * height);
  cs = CGColorSpaceCreateWithName (kCGColorSpaceSRGB);
  ctx = CGBitmapContextCreate (pixels, width, height, 8, stride, cs,
                               kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease (cs);
  if (ctx == NULL)
    {
      g_free (pixels);
      return FALSE;
    }
  CGContextDrawImage (ctx, CGRectMake (0, 0, width, height), image);
  CGContextRelease (ctx);

  /* Quick Look renders documents (text, source code, ...) as opaque white
   * pages without a frame; Finder draws one itself. Without it the page is
   * invisible on Thunar's white view background, so add a subtle frame when
   * the edge pixels are opaque and (mostly, text may touch the edge) white. */
  if (width > 8 && height > 8)
    {
      gsize n_edge = 0, n_white = 0;
      for (gsize y = 0; y < height; ++y)
        for (gsize x = 0; x < width; x += (y == 0 || y == height - 1) ? 1 : width - 1)
          {
            const guchar *p = pixels + y * stride + x * 4;
            if (p[3] != 255)
              {
                n_white = 0;
                y = height;
                break;
              }
            n_edge++;
            if (p[0] >= 240 && p[1] >= 240 && p[2] >= 240)
              n_white++;
          }
      if (n_white * 10 >= n_edge * 9)
        for (gsize y = 0; y < height; ++y)
          for (gsize x = 0; x < width; x += (y == 0 || y == height - 1) ? 1 : width - 1)
            {
              guchar *p = pixels + y * stride + x * 4;
              p[0] = p[1] = p[2] = 0xb4;
            }
    }

  /* GdkPixbuf wants straight (non-premultiplied) alpha */
  for (gsize i = 0; i < stride * height; i += 4)
    {
      guint a = pixels[i + 3];
      if (a != 0 && a != 255)
        for (guint c = 0; c < 3; ++c)
          pixels[i + c] = MIN (255, (pixels[i + c] * 255 + a / 2) / a);
    }

  pixbuf = gdk_pixbuf_new_from_data (pixels, GDK_COLORSPACE_RGB, TRUE, 8, width, height, stride,
                                     (GdkPixbufDestroyNotify) g_free, NULL);

  dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  tmp = g_strdup_printf ("%s.%u.tmp", path, g_random_int ());
  mtime_str = g_strdup_printf ("%" G_GINT64_FORMAT, mtime);
  size_str = g_strdup_printf ("%" G_GINT64_FORMAT, size);

  ok = gdk_pixbuf_save (pixbuf, tmp, "png", NULL,
                        "tEXt::Thumb::URI", uri,
                        "tEXt::Thumb::MTime", mtime_str,
                        "tEXt::Thumb::Size", size_str,
                        "tEXt::Software", "thunar-macos-thumbnailer",
                        NULL);
  if (ok)
    {
      g_chmod (tmp, 0600);
      ok = (g_rename (tmp, path) == 0);
    }
  if (!ok)
    g_unlink (tmp);

  g_object_unref (pixbuf);
  g_free (dir);
  g_free (tmp);
  g_free (mtime_str);
  g_free (size_str);
  return ok;
}



/* ---------------------------------------------------------------------- */
/* request bookkeeping (main thread only)                                 */
/* ---------------------------------------------------------------------- */

static void
request_free (Request *request)
{
  if (request->flush_id != 0)
    g_source_remove (request->flush_id);
  g_ptr_array_unref (request->ready);
  g_ptr_array_unref (request->failed);
  g_free (request->flavor);
  g_free (request);
}



static gboolean
idle_quit (gpointer user_data)
{
  idle_id = 0;
  if (g_hash_table_size (requests) == 0)
    g_main_loop_quit (main_loop);
  return G_SOURCE_REMOVE;
}



static void
reset_idle_timer (void)
{
  if (idle_id != 0)
    g_source_remove (idle_id);
  idle_id = g_timeout_add_seconds (IDLE_TIMEOUT, idle_quit, NULL);
}



static gboolean
request_flush (gpointer user_data)
{
  Request *request = g_hash_table_lookup (requests, GUINT_TO_POINTER (GPOINTER_TO_UINT (user_data)));

  if (request == NULL)
    return G_SOURCE_REMOVE;

  request->flush_id = 0;

  if (!request->started)
    {
      request->started = TRUE;
      tmt_thumbnailer_dbus_emit_started (thumbnailer, request->handle);
    }

  if (request->ready->len > 0)
    {
      g_ptr_array_add (request->ready, NULL);
      tmt_thumbnailer_dbus_emit_ready (thumbnailer, request->handle, (const gchar *const *) request->ready->pdata);
      g_ptr_array_set_size (request->ready, 0);
    }

  if (request->failed->len > 0)
    {
      g_ptr_array_add (request->failed, NULL);
      /* 0 = "unsupported" in tumbler's error codes */
      tmt_thumbnailer_dbus_emit_error (thumbnailer, request->handle, (const gchar *const *) request->failed->pdata,
                                       0, "Quick Look could not create a thumbnail");
      g_ptr_array_set_size (request->failed, 0);
    }

  if (request->remaining == 0)
    {
      tmt_thumbnailer_dbus_emit_finished (thumbnailer, request->handle);
      g_hash_table_remove (requests, GUINT_TO_POINTER (request->handle));
      reset_idle_timer ();
    }

  return G_SOURCE_REMOVE;
}



typedef struct
{
  guint    handle;
  gchar   *uri;
  gboolean success;
  gboolean skipped;
} Result;



static gboolean
deliver_result (gpointer user_data)
{
  Result  *result = user_data;
  Request *request = g_hash_table_lookup (requests, GUINT_TO_POINTER (result->handle));

  if (request != NULL)
    {
      request->remaining--;
      if (!result->skipped)
        g_ptr_array_add (result->success ? request->ready : request->failed, g_strdup (result->uri));

      /* batch signals a little to avoid flooding the bus */
      if (request->flush_id == 0)
        request->flush_id = g_timeout_add (request->remaining == 0 ? 0 : 50, request_flush, GUINT_TO_POINTER (request->handle));
    }

  g_free (result->uri);
  g_free (result);
  return G_SOURCE_REMOVE;
}



static void
post_result (guint        handle,
             const gchar *uri,
             gboolean     success,
             gboolean     skipped)
{
  Result *result = g_new0 (Result, 1);
  result->handle = handle;
  result->uri = g_strdup (uri);
  result->success = success;
  result->skipped = skipped;
  g_main_context_invoke (NULL, deliver_result, result);
}



/* thread safe check (requests are only modified on the main thread, but
 * reading the cancelled flag without a lock is fine for a hint) */
static GMutex cancel_lock;
static GHashTable *cancelled_handles = NULL;

static gboolean
handle_is_cancelled (guint handle)
{
  gboolean cancelled;
  g_mutex_lock (&cancel_lock);
  cancelled = g_hash_table_contains (cancelled_handles, GUINT_TO_POINTER (handle));
  g_mutex_unlock (&cancel_lock);
  return cancelled;
}



/* ---------------------------------------------------------------------- */
/* worker (runs on the NSOperationQueue)                                  */
/* ---------------------------------------------------------------------- */

static void
process_uri (guint        handle,
             const gchar *uri,
             const gchar *flavor,
             gint         size)
{
  gchar   *path;
  gchar   *thumb;
  GStatBuf st;
  gint64   mtime;
  __block gboolean success = FALSE;

  if (handle_is_cancelled (handle))
    {
      post_result (handle, uri, FALSE, TRUE);
      return;
    }

  path = g_filename_from_uri (uri, NULL, NULL);
  if (path == NULL || g_stat (path, &st) != 0 || !S_ISREG (st.st_mode))
    {
      g_free (path);
      post_result (handle, uri, FALSE, FALSE);
      return;
    }
  mtime = st.st_mtime;

  thumb = thumbnail_path (uri, flavor);
  if (thumbnail_is_valid (thumb, uri, mtime))
    {
      post_result (handle, uri, TRUE, FALSE);
      goto out;
    }

  if (fail_is_valid (uri, mtime))
    {
      post_result (handle, uri, FALSE, FALSE);
      goto out;
    }

  @autoreleasepool
    {
      NSURL                         *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
      QLThumbnailGenerationRequest  *req;
      dispatch_semaphore_t           done = dispatch_semaphore_create (0);
      gint64                         file_size = st.st_size;
      gchar                         *thumb_copy = thumb;

      req = [[QLThumbnailGenerationRequest alloc] initWithFileAtURL:url
                                                               size:CGSizeMake (size, size)
                                                              scale:1.0
                                                representationTypes:QLThumbnailGenerationRequestRepresentationTypeThumbnail];

      [[QLThumbnailGenerator sharedGenerator]
        generateBestRepresentationForRequest:req
                           completionHandler:^(QLThumbnailRepresentation *rep, NSError *error) {
                             if (rep != nil && rep.CGImage != NULL)
                               success = thumbnail_save (rep.CGImage, thumb_copy, uri, mtime, file_size);
                             dispatch_semaphore_signal (done);
                           }];

      /* don't hang forever on broken generators */
      if (dispatch_semaphore_wait (done, dispatch_time (DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) != 0)
        [[QLThumbnailGenerator sharedGenerator] cancelRequest:req];
    }

  if (!success)
    fail_record (uri);

  post_result (handle, uri, success, FALSE);

out:
  g_free (thumb);
  g_free (path);
}



/* ---------------------------------------------------------------------- */
/* Thumbnailer1 methods                                                   */
/* ---------------------------------------------------------------------- */

static gboolean
handle_queue (TmtThumbnailerDBus    *object,
              GDBusMethodInvocation *invocation,
              const gchar *const    *uris,
              const gchar *const    *mime_hints,
              const gchar           *flavor,
              const gchar           *scheduler,
              guint                  handle_to_unqueue,
              gpointer               user_data)
{
  Request *request;
  gint     size = flavor_size (flavor);
  guint    n_uris = uris != NULL ? g_strv_length ((gchar **) uris) : 0;

  if (size == 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flavor \"%s\"", flavor);
      return TRUE;
    }

  request = g_new0 (Request, 1);
  request->handle = ++last_handle;
  request->flavor = g_strdup (flavor);
  request->size = size;
  request->remaining = n_uris;
  request->ready = g_ptr_array_new_with_free_func (g_free);
  request->failed = g_ptr_array_new_with_free_func (g_free);
  g_hash_table_insert (requests, GUINT_TO_POINTER (request->handle), request);

  tmt_thumbnailer_dbus_complete_queue (object, invocation, request->handle);

  if (idle_id != 0)
    {
      g_source_remove (idle_id);
      idle_id = 0;
    }

  if (n_uris == 0)
    {
      request->flush_id = g_idle_add (request_flush, GUINT_TO_POINTER (request->handle));
      return TRUE;
    }

  for (guint i = 0; i < n_uris; ++i)
    {
      guint  handle = request->handle;
      gchar *uri = g_strdup (uris[i]);
      gchar *flv = g_strdup (flavor);

      [work_queue addOperationWithBlock:^{
        process_uri (handle, uri, flv, size);
        g_free (uri);
        g_free (flv);
      }];
    }

  return TRUE;
}



static gboolean
handle_dequeue (TmtThumbnailerDBus    *object,
                GDBusMethodInvocation *invocation,
                guint                  handle,
                gpointer               user_data)
{
  g_mutex_lock (&cancel_lock);
  g_hash_table_add (cancelled_handles, GUINT_TO_POINTER (handle));
  g_mutex_unlock (&cancel_lock);

  tmt_thumbnailer_dbus_complete_dequeue (object, invocation);
  return TRUE;
}



static gboolean
handle_get_supported (TmtThumbnailerDBus    *object,
                      GDBusMethodInvocation *invocation,
                      gpointer               user_data)
{
  /* Quick Look decides per file; "*" tells Thunar to ask for every regular
   * local file (failures are cached, see fail_record()) */
  const gchar *schemes[] = { "file", NULL };
  const gchar *types[] = { "*", NULL };

  tmt_thumbnailer_dbus_complete_get_supported (object, invocation, schemes, types);
  return TRUE;
}



static gboolean
handle_get_schedulers (TmtThumbnailerDBus    *object,
                       GDBusMethodInvocation *invocation,
                       gpointer               user_data)
{
  const gchar *schedulers[] = { "default", "foreground", "background", NULL };
  tmt_thumbnailer_dbus_complete_get_schedulers (object, invocation, schedulers);
  return TRUE;
}



/* ---------------------------------------------------------------------- */
/* Cache1 methods                                                         */
/* ---------------------------------------------------------------------- */

static void
cache_transfer (const gchar *const *from_uris,
                const gchar *const *to_uris,
                gboolean            copy)
{
  for (guint i = 0; from_uris != NULL && to_uris != NULL && from_uris[i] != NULL && to_uris[i] != NULL; ++i)
    for (guint f = 0; f < G_N_ELEMENTS (flavors); ++f)
      {
        gchar *from = thumbnail_path (from_uris[i], flavors[f].flavor);
        gchar *to = thumbnail_path (to_uris[i], flavors[f].flavor);

        if (g_file_test (from, G_FILE_TEST_EXISTS))
          {
            /* thumbnails embed the URI; drop them rather than keeping stale metadata
             * for copies, but a move keeps the (still valid) image */
            if (copy)
              {
                GFile *src = g_file_new_for_path (from);
                GFile *dst = g_file_new_for_path (to);
                g_file_copy (src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);
                g_object_unref (src);
                g_object_unref (dst);
              }
            else
              g_rename (from, to);
          }

        g_free (from);
        g_free (to);
      }
}



static gboolean
handle_cache_move (TmtThumbnailCacheDBus *object,
                   GDBusMethodInvocation *invocation,
                   const gchar *const    *from_uris,
                   const gchar *const    *to_uris,
                   gpointer               user_data)
{
  cache_transfer (from_uris, to_uris, FALSE);
  tmt_thumbnail_cache_dbus_complete_move (object, invocation);
  return TRUE;
}



static gboolean
handle_cache_copy (TmtThumbnailCacheDBus *object,
                   GDBusMethodInvocation *invocation,
                   const gchar *const    *from_uris,
                   const gchar *const    *to_uris,
                   gpointer               user_data)
{
  cache_transfer (from_uris, to_uris, TRUE);
  tmt_thumbnail_cache_dbus_complete_copy (object, invocation);
  return TRUE;
}



static gboolean
handle_cache_delete (TmtThumbnailCacheDBus *object,
                     GDBusMethodInvocation *invocation,
                     const gchar *const    *uris,
                     gpointer               user_data)
{
  for (guint i = 0; uris != NULL && uris[i] != NULL; ++i)
    {
      for (guint f = 0; f < G_N_ELEMENTS (flavors); ++f)
        {
          gchar *path = thumbnail_path (uris[i], flavors[f].flavor);
          g_unlink (path);
          g_free (path);
        }
      {
        gchar *path = fail_path (uris[i]);
        g_unlink (path);
        g_free (path);
      }
    }

  tmt_thumbnail_cache_dbus_complete_delete (object, invocation);
  return TRUE;
}



static gboolean
handle_cache_cleanup (TmtThumbnailCacheDBus *object,
                      GDBusMethodInvocation *invocation,
                      const gchar *const    *base_uris,
                      guint                  since,
                      gpointer               user_data)
{
  tmt_thumbnail_cache_dbus_complete_cleanup (object, invocation);
  return TRUE;
}



/* ---------------------------------------------------------------------- */
/* main                                                                   */
/* ---------------------------------------------------------------------- */

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  TmtThumbnailCacheDBus *cache;
  GError                *error = NULL;

  thumbnailer = tmt_thumbnailer_dbus_skeleton_new ();
  g_signal_connect (thumbnailer, "handle-queue", G_CALLBACK (handle_queue), NULL);
  g_signal_connect (thumbnailer, "handle-dequeue", G_CALLBACK (handle_dequeue), NULL);
  g_signal_connect (thumbnailer, "handle-get-supported", G_CALLBACK (handle_get_supported), NULL);
  g_signal_connect (thumbnailer, "handle-get-schedulers", G_CALLBACK (handle_get_schedulers), NULL);
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (thumbnailer), connection, THUMBNAILER_PATH, &error))
    {
      g_printerr ("Failed to export %s: %s\n", THUMBNAILER_PATH, error->message);
      g_clear_error (&error);
    }

  cache = tmt_thumbnail_cache_dbus_skeleton_new ();
  g_signal_connect (cache, "handle-move", G_CALLBACK (handle_cache_move), NULL);
  g_signal_connect (cache, "handle-copy", G_CALLBACK (handle_cache_copy), NULL);
  g_signal_connect (cache, "handle-delete", G_CALLBACK (handle_cache_delete), NULL);
  g_signal_connect (cache, "handle-cleanup", G_CALLBACK (handle_cache_cleanup), NULL);
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (cache), connection, CACHE_PATH, &error))
    {
      g_printerr ("Failed to export %s: %s\n", CACHE_PATH, error->message);
      g_clear_error (&error);
    }
}



static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_printerr ("thunar-macos-thumbnailer: lost (or could not acquire) %s\n", name);
  g_main_loop_quit (main_loop);
}



int
main (int    argc,
      char **argv)
{
  guint owner1, owner2;

  @autoreleasepool
    {
      requests = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) request_free);
      cancelled_handles = g_hash_table_new (g_direct_hash, g_direct_equal);

      work_queue = [[NSOperationQueue alloc] init];
      work_queue.maxConcurrentOperationCount = MAX_PARALLEL;
      work_queue.qualityOfService = NSQualityOfServiceUserInitiated;

      main_loop = g_main_loop_new (NULL, FALSE);

      owner1 = g_bus_own_name (G_BUS_TYPE_SESSION, THUMBNAILER_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                               on_bus_acquired, NULL, on_name_lost, NULL, NULL);
      owner2 = g_bus_own_name (G_BUS_TYPE_SESSION, CACHE_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                               NULL, NULL, NULL, NULL, NULL);

      reset_idle_timer ();
      g_main_loop_run (main_loop);

      g_bus_unown_name (owner1);
      g_bus_unown_name (owner2);
      [work_queue cancelAllOperations];
    }

  return 0;
}
