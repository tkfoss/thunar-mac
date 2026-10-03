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

/* macOS: there is no xfce4-mime-helper / MailReader preferred application,
 * so hand the attachments to the system "Compose Email" sharing service
 * (Mail.app or the configured mail client). Falls back to "open -a Mail". */

#import <AppKit/AppKit.h>

#include <gtk/gtk.h>

#include "tse-macos.h"



@interface TseShareDelegate : NSObject <NSSharingServiceDelegate>
@property (nonatomic) BOOL     done;
@property (nonatomic) BOOL     failed;
@property (nonatomic, copy) NSString *message;
@end

@implementation TseShareDelegate
- (void)sharingService:(NSSharingService *)sharingService didShareItems:(NSArray *)items
{
  self.done = YES;
  gtk_main_quit ();
}

- (void)sharingService:(NSSharingService *)sharingService
    didFailToShareItems:(NSArray *)items
                  error:(NSError *)error
{
  /* user cancel is not an error */
  if (!([error.domain isEqualToString:NSCocoaErrorDomain] && error.code == NSUserCancelledError))
    {
      self.failed = YES;
      self.message = error.localizedDescription;
    }
  self.done = YES;
  gtk_main_quit ();
}
@end



static gboolean
tse_macos_open_with_mail (NSArray<NSURL *> *urls,
                          GError          **error)
{
  NSURL                       *mail;
  NSWorkspaceOpenConfiguration *config;
  __block BOOL                 finished = NO;
  __block NSString            *message = nil;

  mail = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:@"com.apple.mail"];
  if (mail == nil)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Mail.app not found");
      return FALSE;
    }

  config = [NSWorkspaceOpenConfiguration configuration];
  [[NSWorkspace sharedWorkspace] openURLs:urls
                     withApplicationAtURL:mail
                            configuration:config
                        completionHandler:^(NSRunningApplication *app G_GNUC_UNUSED, NSError *err) {
                          if (err != nil)
                            message = [err.localizedDescription copy];
                          finished = YES;
                        }];

  /* the completion handler runs on a background queue */
  while (!finished)
    g_main_context_iteration (NULL, FALSE), g_usleep (10000);

  if (message != nil)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, message.UTF8String);
      return FALSE;
    }
  return TRUE;
}



gboolean
tse_macos_compose_email (gchar  **attachments,
                         GError **error)
{
  @autoreleasepool
    {
      NSMutableArray<NSURL *> *urls = [NSMutableArray array];
      NSSharingService        *service;
      TseShareDelegate        *delegate;
      guint                    n;

      for (n = 0; attachments[n] != NULL; ++n)
        {
          NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:attachments[n]]];
          if (url != nil)
            [urls addObject:url];
        }

      if (urls.count == 0)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No attachments");
          return FALSE;
        }

      service = [NSSharingService sharingServiceNamed:NSSharingServiceNameComposeEmail];
      if (service == nil || ![service canPerformWithItems:urls])
        return tse_macos_open_with_mail (urls, error);

      delegate = [TseShareDelegate new];
      service.delegate = delegate;
      [service performWithItems:urls];

      /* wait until the mail client took the items (or failed) */
      if (!delegate.done)
        gtk_main ();

      if (delegate.failed)
        {
          /* sharing service failed, try Mail.app directly */
          if (tse_macos_open_with_mail (urls, NULL))
            return TRUE;
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               delegate.message != nil ? delegate.message.UTF8String : "Failed to compose email");
          return FALSE;
        }

      return TRUE;
    }
}
