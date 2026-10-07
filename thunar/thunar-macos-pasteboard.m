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

/* NSPasteboard helpers for the clipboard manager and for drags.
 *
 * GTK3's Quartz clipboard can't carry files: custom targets such as
 * x-special/gnome-copied-files are written under a dynamic UTI but read back
 * under their raw name (so the data is never found), and text/uri-list is
 * mapped to a single public.url. Thunar therefore talks to the general
 * pasteboard directly, in the same format Finder uses: one pasteboard item per
 * file with public.file-url. A private type marks "cut" and carries URIs that
 * have no file URL (trash://).
 *
 * Drags have the same text/uri-list -> single public.url problem, so the drag
 * pasteboard is extended with all dragged files right before the drag
 * starts. Called from the GTK main thread only. */

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <string.h>

#include "thunar/thunar-macos.h"

#define THUNAR_PB_OPERATION @"org.xfce.thunar.operation"
#define THUNAR_PB_URI       @"org.xfce.thunar.uri"



static NSString *
path_to_string (const gchar *path)
{
  return [[NSFileManager defaultManager] stringWithFileSystemRepresentation:path
                                                                     length:strlen (path)];
}



/* ---------------------------------------------------------------------- */
/* clipboard                                                              */
/* ---------------------------------------------------------------------- */

glong
thunar_macos_pasteboard_change_count (void)
{
  return (glong) [[NSPasteboard generalPasteboard] changeCount];
}



glong
thunar_macos_pasteboard_write_files (GList   *files,
                                     gboolean cut)
{
  NSPasteboard   *pasteboard = [NSPasteboard generalPasteboard];
  NSMutableArray *items = [NSMutableArray array];
  NSMutableArray *names = [NSMutableArray array];
  GList          *lp;

  for (lp = files; lp != NULL; lp = lp->next)
    {
      NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
      gchar            *uri = g_file_get_uri (G_FILE (lp->data));
      gchar            *path = g_file_get_path (G_FILE (lp->data));
      gchar            *name = g_file_get_parse_name (G_FILE (lp->data));

      if (path != NULL)
        {
          NSURL *url = [NSURL fileURLWithPath:path_to_string (path)];
          [item setString:[url absoluteString] forType:NSPasteboardTypeFileURL];
        }
      else
        {
          /* not a local file (e.g. trash://): only Thunar can paste it */
          [item setString:@(uri) forType:THUNAR_PB_URI];
        }

      if (lp == files)
        [item setString:(cut ? @"cut" : @"copy") forType:THUNAR_PB_OPERATION];

      if (name != NULL && [NSString stringWithUTF8String:name] != nil)
        [names addObject:[NSString stringWithUTF8String:name]];

      [items addObject:item];
      g_free (uri);
      g_free (path);
      g_free (name);
    }

  /* plain text for text editors (Finder puts the file names there) */
  if ([items count] > 0 && [names count] > 0)
    [[items objectAtIndex:0] setString:[names componentsJoinedByString:@"\n"]
                               forType:NSPasteboardTypeString];

  [pasteboard clearContents];
  if ([items count] > 0)
    [pasteboard writeObjects:items];

  return (glong) [pasteboard changeCount];
}



GList *
thunar_macos_pasteboard_read_files (gboolean *cut)
{
  NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
  GList        *files = NULL;
  NSString     *operation = nil;

  for (NSPasteboardItem *item in [pasteboard pasteboardItems])
    {
      NSString *uri = [item stringForType:THUNAR_PB_URI];
      NSString *file_url;
      NSString *op = [item stringForType:THUNAR_PB_OPERATION];

      if (op != nil && operation == nil)
        operation = op;

      if (uri != nil)
        {
          files = g_list_prepend (files, g_file_new_for_uri ([uri UTF8String]));
          continue;
        }

      file_url = [item stringForType:NSPasteboardTypeFileURL];
      if (file_url != nil)
        {
          NSURL *url = [NSURL URLWithString:file_url];

          /* resolves file reference URLs (file:///.file/id=...) as well */
          if (url != nil && [url isFileURL] && [url filePathURL] != nil)
            files = g_list_prepend (files, g_file_new_for_path ([[[url filePathURL] path] fileSystemRepresentation]));
        }
    }

  if (cut != NULL)
    *cut = [operation isEqualToString:@"cut"];

  return g_list_reverse (files);
}



gboolean
thunar_macos_pasteboard_has_files (void)
{
  NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];

  return [pasteboard availableTypeFromArray:@[ NSPasteboardTypeFileURL, THUNAR_PB_URI ]] != nil;
}



/* image types that can be pasted as a new file, best first */
static const struct
{
  const gchar *uti;
  const gchar *mime_type;
} image_types[] = {
  { "public.png", "image/png" },
  { "public.jpeg", "image/jpeg" },
  { "public.tiff", "image/tiff" },
};

gchar *
thunar_macos_pasteboard_image_mime_type (void)
{
  NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
  NSArray      *types = [pasteboard types];
  guint         n;

  /* Finder also puts the file icon on the pasteboard, files win */
  if (thunar_macos_pasteboard_has_files ())
    return NULL;

  for (n = 0; n < G_N_ELEMENTS (image_types); ++n)
    if ([types containsObject:[NSString stringWithUTF8String:image_types[n].uti]])
      return g_strdup (image_types[n].mime_type);

  return NULL;
}



GBytes *
thunar_macos_pasteboard_read_image (const gchar *mime_type)
{
  NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
  NSData       *data;
  guint         n;

  for (n = 0; n < G_N_ELEMENTS (image_types); ++n)
    if (g_strcmp0 (mime_type, image_types[n].mime_type) == 0)
      {
        data = [pasteboard dataForType:[NSString stringWithUTF8String:image_types[n].uti]];
        if (data != nil)
          return g_bytes_new ([data bytes], [data length]);
      }

  return NULL;
}



void
thunar_macos_pasteboard_clear (glong change_count)
{
  NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];

  /* only clear our own contents */
  if ((glong) [pasteboard changeCount] == change_count)
    [pasteboard clearContents];
}



/* ---------------------------------------------------------------------- */
/* drags                                                                  */
/* ---------------------------------------------------------------------- */

/* paths of the files dragged from a Thunar view, set on drag-begin */
static NSArray *drag_paths = nil;

static IMP original_drag_image = NULL;

typedef void (*DragImageFunc) (id, SEL, NSImage *, NSPoint, NSSize, NSEvent *, NSPasteboard *, id, BOOL);

static void
thunar_drag_image (id            self,
                   SEL           cmd,
                   NSImage      *image,
                   NSPoint       point,
                   NSSize        offset,
                   NSEvent      *event,
                   NSPasteboard *pasteboard,
                   id            source,
                   BOOL          slide_back)
{
  /* GTK declared the targets of the drag (text/uri-list becomes a single
   * public.url) right before. Add all dragged files, which the pasteboard
   * turns into one public.file-url item per file, so Finder and GTK's own
   * text/uri-list reader (which prefers NSFilenamesPboardType) see them all. */
  if (drag_paths != nil && [drag_paths count] > 0
      && [[pasteboard name] isEqualToString:NSPasteboardNameDrag])
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      [pasteboard addTypes:@[ NSFilenamesPboardType ] owner:nil];
      [pasteboard setPropertyList:drag_paths forType:NSFilenamesPboardType];
#pragma clang diagnostic pop
    }

  ((DragImageFunc) original_drag_image) (self, cmd, image, point, offset, event, pasteboard, source, slide_back);
}



/* operations a drag from Thunar allows; GTK passes the window as the legacy
 * dragging source but doesn't implement this, so AppKit would fall back to a
 * default that may not include Move */
static NSDragOperation
thunar_drag_source_operation_mask (id   self,
                                   SEL  cmd,
                                   BOOL local)
{
  (void) self;
  (void) cmd;
  (void) local;
  return NSDragOperationCopy | NSDragOperationMove | NSDragOperationLink | NSDragOperationGeneric;
}



static void
install_drag_hook (void)
{
  Method method;
  SEL    selector = @selector (dragImage:at:offset:event:pasteboard:source:slideBack:);
  SEL    mask_selector = @selector (draggingSourceOperationMaskForLocal:);

  if (original_drag_image != NULL)
    return;

  /* GdkQuartzNSWindow inherits this from NSWindow, used by gtkdnd-quartz.c */
  method = class_getInstanceMethod ([NSWindow class], selector);
  if (method != NULL)
    original_drag_image = method_setImplementation (method, (IMP) thunar_drag_image);

  if (![NSWindow instancesRespondToSelector:mask_selector])
    {
      char types[16];
      g_snprintf (types, sizeof (types), "%s%s%s%s", @encode (NSDragOperation), @encode (id), @encode (SEL), @encode (BOOL));
      class_addMethod ([NSWindow class], mask_selector, (IMP) thunar_drag_source_operation_mask, types);
    }
}



void
thunar_macos_drag_set_files (GList *files)
{
  NSMutableArray *paths;
  GList          *lp;

  install_drag_hook ();

  drag_paths = nil;
  if (files == NULL)
    return;

  paths = [NSMutableArray array];
  for (lp = files; lp != NULL; lp = lp->next)
    {
      gchar *path = g_file_get_path (G_FILE (lp->data));
      if (path != NULL)
        [paths addObject:path_to_string (path)];
      g_free (path);
    }

  if ([paths count] > 0)
    drag_paths = paths;
}
