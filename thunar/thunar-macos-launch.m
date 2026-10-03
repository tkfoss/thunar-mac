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

/* LaunchServices / NSWorkspace helpers for ThunarMacosAppInfo:
 * application lookup per content type (UTI), default handlers, launching
 * and application icons. Called from the GTK main thread only. */

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "thunar/thunar-macos.h"



static UTType *
type_for_content_type (const gchar *content_type)
{
  NSString *identifier;
  UTType   *type;

  if (content_type == NULL)
    return nil;

  identifier = [NSString stringWithUTF8String:content_type];
  type = [UTType typeWithIdentifier:identifier];

  /* MIME types (should not happen, but be tolerant) */
  if (type == nil && strchr (content_type, '/') != NULL)
    type = [UTType typeWithMIMEType:identifier];

  return type;
}



static gchar **
strv_from_urls (NSArray<NSURL *> *urls)
{
  GPtrArray *array = g_ptr_array_new ();
  GHashTable *seen = g_hash_table_new (g_str_hash, g_str_equal);

  for (NSURL *url in urls)
    {
      const gchar *path = url.path.fileSystemRepresentation;
      if (path == NULL || g_hash_table_contains (seen, path))
        continue;
      gchar *dup = g_strdup (path);
      g_hash_table_add (seen, dup);
      g_ptr_array_add (array, dup);
    }

  g_hash_table_destroy (seen);
  g_ptr_array_add (array, NULL);
  return (gchar **) g_ptr_array_free (array, FALSE);
}



gchar **
thunar_macos_apps_for_content_type (const gchar *content_type)
{
  @autoreleasepool
    {
      UTType *type = type_for_content_type (content_type);
      if (type == nil)
        return g_new0 (gchar *, 1);
      return strv_from_urls ([[NSWorkspace sharedWorkspace] URLsForApplicationsToOpenContentType:type]);
    }
}



gchar *
thunar_macos_default_app_for_content_type (const gchar *content_type)
{
  @autoreleasepool
    {
      UTType *type = type_for_content_type (content_type);
      NSURL  *url;

      if (type == nil)
        return NULL;

      url = [[NSWorkspace sharedWorkspace] URLForApplicationToOpenContentType:type];
      return url != nil ? g_strdup (url.path.fileSystemRepresentation) : NULL;
    }
}



gboolean
thunar_macos_set_default_app_for_content_type (const gchar *app_path,
                                               const gchar *content_type,
                                               GError     **error)
{
  @autoreleasepool
    {
      UTType                *type = type_for_content_type (content_type);
      dispatch_semaphore_t   done;
      __block NSError       *result = nil;

      if (type == nil)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "Unknown content type \"%s\"", content_type);
          return FALSE;
        }

      done = dispatch_semaphore_create (0);
      [[NSWorkspace sharedWorkspace] setDefaultApplicationAtURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:app_path]]
                                              toOpenContentType:type
                                              completionHandler:^(NSError *err) {
                                                result = err;
                                                dispatch_semaphore_signal (done);
                                              }];

      /* the completion handler runs on a private queue, so waiting is fine */
      if (dispatch_semaphore_wait (done, dispatch_time (DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                       "Timeout while changing the default application");
          return FALSE;
        }

      if (result != nil)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                       result.localizedDescription.UTF8String);
          return FALSE;
        }

      return TRUE;
    }
}



gchar *
thunar_macos_app_path_for_bundle_id (const gchar *bundle_id)
{
  @autoreleasepool
    {
      NSURL *url = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:[NSString stringWithUTF8String:bundle_id]];
      return url != nil ? g_strdup (url.path.fileSystemRepresentation) : NULL;
    }
}



gboolean
thunar_macos_app_bundle_info (const gchar *app_path,
                              gchar      **bundle_id,
                              gchar      **display_name,
                              gchar      **executable)
{
  @autoreleasepool
    {
      NSString *path = [NSString stringWithUTF8String:app_path];
      NSBundle *bundle = [NSBundle bundleWithPath:path];
      NSString *name;

      if (bundle == nil)
        return FALSE;

      if (bundle_id != NULL)
        *bundle_id = bundle.bundleIdentifier != nil ? g_strdup (bundle.bundleIdentifier.UTF8String) : NULL;

      if (display_name != NULL)
        {
          /* localized and without ".app", like Finder shows it */
          name = [[NSFileManager defaultManager] displayNameAtPath:path];
          if ([name hasSuffix:@".app"])
            name = [name stringByDeletingPathExtension];
          *display_name = g_strdup (name.UTF8String);
        }

      if (executable != NULL)
        *executable = bundle.executablePath != nil ? g_strdup (bundle.executablePath.fileSystemRepresentation) : NULL;

      return TRUE;
    }
}



static void
scan_app_dir (NSString  *dir,
              gint       depth,
              GPtrArray *result)
{
  NSFileManager *fm = [NSFileManager defaultManager];
  NSArray       *entries = [fm contentsOfDirectoryAtPath:dir error:nil];

  for (NSString *entry in entries)
    {
      NSString *path;
      BOOL      is_dir = NO;

      if ([entry hasPrefix:@"."])
        continue;

      path = [dir stringByAppendingPathComponent:entry];
      if ([entry.pathExtension caseInsensitiveCompare:@"app"] == NSOrderedSame)
        g_ptr_array_add (result, g_strdup (path.fileSystemRepresentation));
      else if (depth > 0 && [fm fileExistsAtPath:path isDirectory:&is_dir] && is_dir)
        scan_app_dir (path, depth - 1, result);
    }
}



gchar **
thunar_macos_all_apps (void)
{
  @autoreleasepool
    {
      GPtrArray *result = g_ptr_array_new ();
      NSArray   *dirs = @[ @"/Applications",
                         @"/System/Applications",
                         @"/System/Library/CoreServices/Applications",
                         [NSHomeDirectory () stringByAppendingPathComponent:@"Applications"] ];

      for (NSString *dir in dirs)
        scan_app_dir (dir, 2, result);

      /* Finder lives in CoreServices directly */
      g_ptr_array_add (result, g_strdup ("/System/Library/CoreServices/Finder.app"));

      g_ptr_array_add (result, NULL);
      return (gchar **) g_ptr_array_free (result, FALSE);
    }
}



GBytes *
thunar_macos_file_icon_png (const gchar *path,
                            gint         size)
{
  @autoreleasepool
    {
      NSImage          *image = [[NSWorkspace sharedWorkspace] iconForFile:[NSString stringWithUTF8String:path]];
      NSBitmapImageRep *rep;
      NSData           *png;

      if (image == nil || size <= 0)
        return NULL;

      rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                    pixelsWide:size
                                                    pixelsHigh:size
                                                 bitsPerSample:8
                                               samplesPerPixel:4
                                                      hasAlpha:YES
                                                      isPlanar:NO
                                                colorSpaceName:NSDeviceRGBColorSpace
                                                   bytesPerRow:0
                                                  bitsPerPixel:0];
      rep.size = NSMakeSize (size, size);

      [NSGraphicsContext saveGraphicsState];
      [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];
      [image drawInRect:NSMakeRect (0, 0, size, size)
               fromRect:NSZeroRect
              operation:NSCompositingOperationCopy
               fraction:1.0];
      [NSGraphicsContext restoreGraphicsState];

      png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
      if (png == nil)
        return NULL;

      return g_bytes_new (png.bytes, png.length);
    }
}



typedef struct
{
  ThunarMacosLaunchCallback callback;
  GError                   *error;
  gpointer                  user_data;
} LaunchResult;

static gboolean
launch_result_deliver (gpointer data)
{
  LaunchResult *result = data;
  result->callback (result->error, result->user_data);
  g_clear_error (&result->error);
  g_free (result);
  return G_SOURCE_REMOVE;
}



gboolean
thunar_macos_open_with_app (const gchar              *app_path,
                            const gchar *const       *uris,
                            ThunarMacosLaunchCallback callback,
                            gpointer                  user_data)
{
  @autoreleasepool
    {
      NSURL                       *app_url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:app_path]];
      NSWorkspaceOpenConfiguration *config = [NSWorkspaceOpenConfiguration configuration];
      NSMutableArray<NSURL *>     *urls = [NSMutableArray array];
      void (^handler) (NSRunningApplication *, NSError *);

      config.activates = YES;
      config.promptsUserIfNeeded = YES;

      for (guint n = 0; uris != NULL && uris[n] != NULL; n++)
        {
          NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:uris[n]]];
          if (url != nil)
            [urls addObject:url];
        }

      handler = ^(NSRunningApplication *app, NSError *err) {
        (void) app;
        if (callback != NULL)
          {
            /* runs on a private queue, hop back to the GTK main loop */
            LaunchResult *result = g_new0 (LaunchResult, 1);
            result->callback = callback;
            result->user_data = user_data;
            if (err != nil)
              result->error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "%s", err.localizedDescription.UTF8String);
            g_idle_add (launch_result_deliver, result);
          }
      };

      if (urls.count > 0)
        [[NSWorkspace sharedWorkspace] openURLs:urls withApplicationAtURL:app_url configuration:config completionHandler:handler];
      else
        [[NSWorkspace sharedWorkspace] openApplicationAtURL:app_url configuration:config completionHandler:handler];

      return TRUE;
    }
}



static gboolean finished_launching = FALSE;

void
thunar_macos_watch_launch (void)
{
  [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidFinishLaunchingNotification
                                                    object:nil
                                                     queue:nil
                                                usingBlock:^(NSNotification *note) {
                                                  (void) note;
                                                  finished_launching = TRUE;
                                                }];
}



gboolean
thunar_macos_finished_launching (void)
{
  return finished_launching;
}
