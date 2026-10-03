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

/* Volume metadata and unmount/eject through Foundation, used by the macOS
 * volume monitor (thunar-macos-volume-monitor.c). */

#import <Foundation/Foundation.h>

#include "thunar/thunar-macos.h"



gboolean
thunar_macos_volume_info (const gchar *mount_path,
                          gchar      **name,
                          gchar      **uuid,
                          gboolean    *ejectable,
                          gboolean    *local)
{
  @autoreleasepool
    {
      NSURL        *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:mount_path] isDirectory:YES];
      NSArray      *keys = @[ NSURLVolumeLocalizedNameKey, NSURLVolumeUUIDStringKey, NSURLVolumeIsEjectableKey,
                              NSURLVolumeIsRemovableKey, NSURLVolumeIsLocalKey ];
      NSDictionary *values = [url resourceValuesForKeys:keys error:nil];

      if (values == nil)
        return FALSE;

      if (name != NULL)
        *name = values[NSURLVolumeLocalizedNameKey] != nil ? g_strdup ([values[NSURLVolumeLocalizedNameKey] UTF8String]) : NULL;
      if (uuid != NULL)
        *uuid = values[NSURLVolumeUUIDStringKey] != nil ? g_strdup ([values[NSURLVolumeUUIDStringKey] UTF8String]) : NULL;
      if (ejectable != NULL)
        *ejectable = [values[NSURLVolumeIsEjectableKey] boolValue] || [values[NSURLVolumeIsRemovableKey] boolValue];
      if (local != NULL)
        *local = values[NSURLVolumeIsLocalKey] == nil || [values[NSURLVolumeIsLocalKey] boolValue];

      return TRUE;
    }
}



typedef struct
{
  ThunarMacosLaunchCallback callback;
  GError                   *error;
  gpointer                  user_data;
} UnmountResult;

static gboolean
unmount_result_deliver (gpointer data)
{
  UnmountResult *result = data;
  result->callback (result->error, result->user_data);
  g_clear_error (&result->error);
  g_free (result);
  return G_SOURCE_REMOVE;
}



void
thunar_macos_volume_unmount (const gchar              *mount_path,
                             gboolean                  eject,
                             ThunarMacosLaunchCallback callback,
                             gpointer                  user_data)
{
  @autoreleasepool
    {
      NSURL                       *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:mount_path] isDirectory:YES];
      NSFileManagerUnmountOptions options = eject ? NSFileManagerUnmountAllPartitionsAndEjectDisk : 0;

      [[NSFileManager defaultManager]
          unmountVolumeAtURL:url
                     options:options
           completionHandler:^(NSError *err) {
             /* runs on a private queue, hop back to the GTK main loop */
             UnmountResult *result = g_new0 (UnmountResult, 1);
             result->callback = callback;
             result->user_data = user_data;
             if (err != nil)
               {
                 NSString *message = err.localizedFailureReason ?: err.localizedDescription;
                 /* EBUSY: a process has open files on the volume */
                 gint code = ([err.domain isEqualToString:NSPOSIXErrorDomain] && err.code == EBUSY)
                                 || [err.userInfo[NSUnderlyingErrorKey] code] == EBUSY
                                 ? G_IO_ERROR_BUSY
                                 : G_IO_ERROR_FAILED;
                 result->error = g_error_new (G_IO_ERROR, code, "%s", message.UTF8String);
               }
             g_idle_add (unmount_result_deliver, result);
           }];
    }
}
