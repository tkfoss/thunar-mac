/* vi:set et ai sw=2 sts=2 ts=2: */
/*-
 * Copyright (c) 2005-2006 Benedikt Meurer <benny@xfce.org>
 * Copyright (c) 2009-2011 Jannis Pohlmann <jannis@xfce.org>
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
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_MEMORY_H
#include <memory.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "thunar-util.h"
#include "thunar/thunar-application.h"
#include "thunar/thunar-clipboard-manager.h"
#include "thunar/thunar-dialogs.h"
#include "thunar/thunar-gobject-extensions.h"
#include "thunar/thunar-private.h"

#ifdef __APPLE__
#include "thunar/thunar-gio-extensions.h"
#include "thunar/thunar-macos.h"
#endif

#include <libxfce4util/libxfce4util.h>



static const char *IMAGE_TARGET_STRING = "image/";
static const int   IMAGE_TARGET_LEN = 6;

enum
{
  PROP_0,
  PROP_CAN_PASTE,
};

enum
{
  CHANGED,
  LAST_SIGNAL,
};

enum
{
  TARGET_TEXT_URI_LIST,
  TARGET_GNOME_COPIED_FILES,
  TARGET_UTF8_STRING,
};



static void
thunar_clipboard_manager_finalize (GObject *object);
static void
thunar_clipboard_manager_dispose (GObject *object);
static void
thunar_clipboard_manager_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec);
static void
thunar_clipboard_manager_file_destroyed (ThunarFile             *file,
                                         ThunarClipboardManager *manager);
static void
thunar_clipboard_manager_owner_changed (GtkClipboard           *clipboard,
                                        GdkEventOwnerChange    *event,
                                        ThunarClipboardManager *manager);
static void
thunar_clipboard_manager_contents_received (GtkClipboard     *clipboard,
                                            GtkSelectionData *selection_data,
                                            gpointer          user_data);
void
thunar_clipboard_manager_image_received (GtkClipboard     *clipboard,
                                         GtkSelectionData *selection_data,
                                         gpointer          data);
static void
thunar_clipboard_manager_targets_received (GtkClipboard     *clipboard,
                                           GtkSelectionData *selection_data,
                                           gpointer          user_data);
static void
thunar_clipboard_manager_get_callback (GtkClipboard     *clipboard,
                                       GtkSelectionData *selection_data,
                                       guint             info,
                                       gpointer          user_data);
static void
thunar_clipboard_manager_clear_callback (GtkClipboard *clipboard,
                                         gpointer      user_data);
static void
thunar_clipboard_manager_transfer_files (ThunarClipboardManager *manager,
                                         gboolean                copy,
                                         GList                  *files);
static void
thunar_clipboard_manager_release_files (ThunarClipboardManager *manager);
#ifdef __APPLE__
static void
thunar_clipboard_manager_macos_update (ThunarClipboardManager *manager,
                                       gboolean                force);
#endif



struct _ThunarClipboardManagerClass
{
  GObjectClass __parent__;

  void (*changed) (ThunarClipboardManager *manager);
};

struct _ThunarClipboardManager
{
  GObject __parent__;

  GtkClipboard *clipboard;
  gboolean      can_paste;
  GdkAtom       x_special_gnome_copied_files;
  GdkAtom       image_target; /* NULL except when there is a image that can be pasted */

  gboolean files_cutted;
  GList   *files;

#ifdef __APPLE__
  /* GTK's Quartz clipboard can't transfer files, so the NSPasteboard is used
   * directly. It has no change notification, its changeCount is polled. */
  glong  pb_change_count; /* changeCount seen last */
  glong  pb_owned_count;  /* changeCount after we put files on it */
  guint  pb_poll_id;
  gchar *pb_image_mime_type; /* image (not a file) on the pasteboard */
#endif
};

typedef struct
{
  ThunarClipboardManager *manager;
  GFile                  *target_file;
  GtkWidget              *widget;
  GClosure               *new_files_closure;
  gboolean                paste_as_link;
} ThunarClipboardPasteRequest;



static const GtkTargetEntry clipboard_targets[] = {
  { "text/uri-list", 0, TARGET_TEXT_URI_LIST },
  { "x-special/gnome-copied-files", 0, TARGET_GNOME_COPIED_FILES },
  { "UTF8_STRING", 0, TARGET_UTF8_STRING }
};

static GQuark thunar_clipboard_manager_quark = 0;
static guint  manager_signals[LAST_SIGNAL];



G_DEFINE_TYPE (ThunarClipboardManager, thunar_clipboard_manager, G_TYPE_OBJECT)



static void
thunar_clipboard_manager_class_init (ThunarClipboardManagerClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = thunar_clipboard_manager_dispose;
  gobject_class->finalize = thunar_clipboard_manager_finalize;
  gobject_class->get_property = thunar_clipboard_manager_get_property;

  /**
   * ThunarClipboardManager:can-paste:
   *
   * This property tells whether the current clipboard content of
   * this #ThunarClipboardManager can be pasted into a folder
   * displayed by a #ThunarView.
   **/
  g_object_class_install_property (gobject_class,
                                   PROP_CAN_PASTE,
                                   g_param_spec_boolean ("can-paste", "can-paste", "can-paste",
                                                         FALSE,
                                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * ThunarClipboardManager::changed:
   * @manager : a #ThunarClipboardManager.
   *
   * This signal is emitted whenever the contents of the
   * clipboard associated with @manager changes.
   **/
  manager_signals[CHANGED] =
  g_signal_new (I_ ("changed"),
                G_TYPE_FROM_CLASS (klass),
                G_SIGNAL_RUN_FIRST,
                G_STRUCT_OFFSET (ThunarClipboardManagerClass, changed),
                NULL, NULL,
                g_cclosure_marshal_VOID__VOID,
                G_TYPE_NONE, 0);
}



static void
thunar_clipboard_manager_init (ThunarClipboardManager *manager)
{
  manager->x_special_gnome_copied_files = gdk_atom_intern_static_string ("x-special/gnome-copied-files");
  manager->image_target = NULL;
}



static void
thunar_clipboard_manager_dispose (GObject *object)
{
  ThunarClipboardManager *manager = THUNAR_CLIPBOARD_MANAGER (object);

#ifdef __APPLE__
  /* the pasteboard already holds the data, nothing to store */
  if (manager->pb_poll_id != 0)
    {
      g_source_remove (manager->pb_poll_id);
      manager->pb_poll_id = 0;
    }
#else
  /* store the clipboard if we still own it and a clipboard
   * manager is running (gtk_clipboard_store checks this) */
  if (gtk_clipboard_get_owner (manager->clipboard) == object
      && manager->files != NULL)
    {
      gtk_clipboard_set_can_store (manager->clipboard, clipboard_targets,
                                   G_N_ELEMENTS (clipboard_targets));

      gtk_clipboard_store (manager->clipboard);
    }
#endif

  (*G_OBJECT_CLASS (thunar_clipboard_manager_parent_class)->dispose) (object);
}



static void
thunar_clipboard_manager_finalize (GObject *object)
{
  ThunarClipboardManager *manager = THUNAR_CLIPBOARD_MANAGER (object);
  GList                  *lp;

  /* release any pending files */
  for (lp = manager->files; lp != NULL; lp = lp->next)
    {
      g_signal_handlers_disconnect_by_func (G_OBJECT (lp->data), thunar_clipboard_manager_file_destroyed, manager);
      g_object_unref (G_OBJECT (lp->data));
    }
  g_list_free (manager->files);

#ifdef __APPLE__
  g_free (manager->pb_image_mime_type);
#endif

  /* disconnect from the clipboard */
  g_signal_handlers_disconnect_by_func (G_OBJECT (manager->clipboard), thunar_clipboard_manager_owner_changed, manager);
  g_object_set_qdata (G_OBJECT (manager->clipboard), thunar_clipboard_manager_quark, NULL);
  g_object_unref (G_OBJECT (manager->clipboard));

  (*G_OBJECT_CLASS (thunar_clipboard_manager_parent_class)->finalize) (object);
}



static void
thunar_clipboard_manager_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  ThunarClipboardManager *manager = THUNAR_CLIPBOARD_MANAGER (object);

  switch (prop_id)
    {
    case PROP_CAN_PASTE:
      g_value_set_boolean (value, thunar_clipboard_manager_get_can_paste (manager));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}



static void
thunar_clipboard_manager_file_destroyed (ThunarFile             *file,
                                         ThunarClipboardManager *manager)
{
  _thunar_return_if_fail (THUNAR_IS_CLIPBOARD_MANAGER (manager));
  _thunar_return_if_fail (g_list_find (manager->files, file) != NULL);

  /* remove the file from our list */
  manager->files = g_list_remove (manager->files, file);

  /* disconnect from the file */
  g_signal_handlers_disconnect_by_func (G_OBJECT (file), thunar_clipboard_manager_file_destroyed, manager);
  g_object_unref (G_OBJECT (file));
}



static void
thunar_clipboard_manager_owner_changed (GtkClipboard           *clipboard,
                                        GdkEventOwnerChange    *event,
                                        ThunarClipboardManager *manager)
{
  _thunar_return_if_fail (GTK_IS_CLIPBOARD (clipboard));
  _thunar_return_if_fail (THUNAR_IS_CLIPBOARD_MANAGER (manager));
  _thunar_return_if_fail (manager->clipboard == clipboard);

  /* need to take a reference on the manager, because the clipboards
   * "targets received callback" mechanism is not cancellable.
   */
  g_object_ref (G_OBJECT (manager));

  /* request the list of supported targets from the new owner */
  gtk_clipboard_request_contents (clipboard, gdk_atom_intern_static_string ("TARGETS"),
                                  thunar_clipboard_manager_targets_received, manager);
}



static void
thunar_clipboard_paste_request_free (ThunarClipboardPasteRequest *request)
{
  /* free the request */
  if (G_LIKELY (request->widget != NULL))
    g_object_remove_weak_pointer (G_OBJECT (request->widget), (gpointer) &request->widget);
  if (G_LIKELY (request->new_files_closure != NULL))
    g_closure_unref (request->new_files_closure);
  g_object_unref (G_OBJECT (request->manager));
  g_object_unref (request->target_file);
  g_slice_free (ThunarClipboardPasteRequest, request);
}



/* copies/moves/links @file_list as requested, returns FALSE if the list is empty */
static gboolean
thunar_clipboard_manager_paste_file_list (ThunarClipboardPasteRequest *request,
                                          GList                       *file_list,
                                          gboolean                     path_copy)
{
  ThunarApplication *application;

  if (G_UNLIKELY (file_list == NULL))
    {
      /* tell the user that we cannot paste */
      thunar_dialogs_show_error (request->widget, NULL, _("There is nothing on the clipboard to paste"));
      return FALSE;
    }

  application = thunar_application_get ();
  if (G_UNLIKELY (request->paste_as_link))
    thunar_application_link_into (application, request->widget, file_list,
                                  request->target_file, THUNAR_OPERATION_LOG_OPERATIONS, request->new_files_closure);
  else if (G_LIKELY (path_copy))
    thunar_application_copy_into (application, request->widget, file_list, request->target_file, THUNAR_OPERATION_LOG_OPERATIONS, request->new_files_closure);
  else
    thunar_application_move_into (application, request->widget, file_list, request->target_file, THUNAR_OPERATION_LOG_OPERATIONS, request->new_files_closure);
  g_object_unref (G_OBJECT (application));

  return TRUE;
}



static void
thunar_clipboard_manager_contents_received (GtkClipboard     *clipboard,
                                            GtkSelectionData *selection_data,
                                            gpointer          user_data)
{
  ThunarClipboardPasteRequest *request = user_data;
  ThunarClipboardManager      *manager = THUNAR_CLIPBOARD_MANAGER (request->manager);
  gboolean                     path_copy = TRUE;
  GList                       *file_list = NULL;
  gchar                       *data;

  /* check whether the retrieval worked */
  if (G_LIKELY (gtk_selection_data_get_length (selection_data) > 0))
    {
      /* be sure the selection data is zero-terminated */
      data = (gchar *) gtk_selection_data_get_data (selection_data);
      data[gtk_selection_data_get_length (selection_data)] = '\0';

      /* check whether to copy or move */
      if (g_ascii_strncasecmp (data, "copy\n", 5) == 0)
        {
          path_copy = TRUE;
          data += 5;
        }
      else if (g_ascii_strncasecmp (data, "cut\n", 4) == 0)
        {
          path_copy = FALSE;
          data += 4;
        }

      /* determine the path list stored with the selection */
      file_list = thunar_g_file_list_new_from_string (data);
    }

  /* perform the action if possible */
  if (thunar_clipboard_manager_paste_file_list (request, file_list, path_copy))
    {
      thunar_g_list_free_full (file_list);

      /* clear the clipboard if it contained "cutted data"
       * (gtk_clipboard_clear takes care of not clearing
       * the selection if we don't own it)
       */
      if (G_UNLIKELY (!path_copy))
        gtk_clipboard_clear (manager->clipboard);

      /* check the contents of the clipboard again if either the Xserver or
       * our GTK+ version doesn't support the XFixes extension */
      if (!gdk_display_supports_selection_notification (gtk_clipboard_get_display (manager->clipboard)))
        {
          thunar_clipboard_manager_owner_changed (manager->clipboard, NULL, manager);
        }
    }

  thunar_clipboard_paste_request_free (request);
}



/* saves the image @content of @mime_type_name as a new file in the target folder */
static void
thunar_clipboard_manager_save_image (ThunarClipboardPasteRequest *request,
                                     const gchar                 *mime_type_name,
                                     const gchar                 *content,
                                     gsize                        length)
{
  GError          *error = NULL;
  g_autofree char *cwd_name = g_file_get_path (request->target_file);
  g_autoptr (GFile) cwd = NULL;
  g_autoptr (ThunarFile) current_dir = NULL;
  g_autofree char *filename_tmp = NULL;
  g_autofree char *filename = NULL;
  g_autofree char *dest_path = NULL;
  g_autoptr (GFile) dest = NULL;
  g_autoptr (GFileOutputStream) output_stream = NULL;
  const char *image_type = NULL;

  if (mime_type_name == NULL || g_ascii_strncasecmp (IMAGE_TARGET_STRING, mime_type_name, IMAGE_TARGET_LEN) != 0)
    {
      g_warning ("Tried to paste image but data was not an image");
      return;
    }

  if (cwd_name == NULL || content == NULL)
    return;

  cwd = g_file_new_for_path (cwd_name);
  current_dir = thunar_file_get (cwd, NULL);
  if (current_dir == NULL)
    return;

  image_type = mime_type_name + IMAGE_TARGET_LEN;
  filename_tmp = g_strconcat (_("Selection."), image_type, NULL);
  filename = thunar_util_next_new_file_name (current_dir, filename_tmp, THUNAR_NEXT_FILE_NAME_MODE_COPY, FALSE);
  dest_path = g_strconcat (cwd_name, "/", filename, NULL);
  dest = g_file_new_for_path (dest_path);

  output_stream = g_file_create (dest, G_FILE_CREATE_NONE, NULL, &error);

  if (error != NULL)
    {
      g_warning ("%s", error->message);
      g_clear_error (&error);
      return;
    }

  if (output_stream != NULL)
    {
      if (g_output_stream_write_all (G_OUTPUT_STREAM (output_stream), content, length, NULL, NULL, NULL))
        {
          g_output_stream_close (G_OUTPUT_STREAM (output_stream), NULL, NULL);
        }
    }
}



void
thunar_clipboard_manager_image_received (GtkClipboard     *clipboard,
                                         GtkSelectionData *selection_data,
                                         gpointer          data)
{
  ThunarClipboardPasteRequest *request = data;
  g_autofree char             *mime_type_name = NULL;
  gint                         length = gtk_selection_data_get_length (selection_data);

  if (request->manager->image_target != NULL)
    mime_type_name = gdk_atom_name (request->manager->image_target);

  if (length >= 0)
    thunar_clipboard_manager_save_image (request, mime_type_name,
                                         (const gchar *) gtk_selection_data_get_data (selection_data), length);

  thunar_clipboard_paste_request_free (request);
}



static void
thunar_clipboard_manager_targets_received (GtkClipboard     *clipboard,
                                           GtkSelectionData *selection_data,
                                           gpointer          user_data)
{
  ThunarClipboardManager *manager = THUNAR_CLIPBOARD_MANAGER (user_data);
  GdkAtom                *targets;
  gint                    n_targets;
  gint                    n;

  _thunar_return_if_fail (GTK_IS_CLIPBOARD (clipboard));
  _thunar_return_if_fail (THUNAR_IS_CLIPBOARD_MANAGER (manager));
  _thunar_return_if_fail (manager->clipboard == clipboard);

  /* reset the "can-paste" and "image" state */
  manager->can_paste = FALSE;
  manager->image_target = NULL;

  /* check the list of targets provided by the owner */
  if (gtk_selection_data_get_targets (selection_data, &targets, &n_targets))
    {
      for (n = 0; n < n_targets; ++n)
        {
          if (targets[n] == manager->x_special_gnome_copied_files)
            {
              manager->can_paste = TRUE;
              break;
            }

          g_autofree gchar *target_name = gdk_atom_name (targets[n]);
          if (g_ascii_strncasecmp (IMAGE_TARGET_STRING, target_name, IMAGE_TARGET_LEN) == 0)
            {
              manager->image_target = targets[n];
              break;
            }
        }

      g_free (targets);
    }

  /* notify listeners that we have a new clipboard state */
  g_signal_emit (manager, manager_signals[CHANGED], 0);
  g_object_notify (G_OBJECT (manager), "can-paste");

  /* drop the reference taken for the callback */
  g_object_unref (manager);
}



static gchar *
thunar_clipboard_manager_g_file_list_to_string (GList       *list,
                                                const gchar *prefix,
                                                gboolean     format_for_text,
                                                gsize       *len)
{
  GString *string;
  gchar   *tmp;
  GList   *lp;

  /* allocate initial string */
  string = g_string_new (prefix);

  for (lp = list; lp != NULL; lp = lp->next)
    {
      if (format_for_text)
        tmp = g_file_get_parse_name (G_FILE (lp->data));
      else
        tmp = g_file_get_uri (G_FILE (lp->data));

      string = g_string_append (string, tmp);
      g_free (tmp);

      if (lp->next != NULL)
        string = g_string_append_c (string, '\n');
    }

  if (len != NULL)
    *len = string->len;

  return g_string_free (string, FALSE);
}



static void
thunar_clipboard_manager_get_callback (GtkClipboard     *clipboard,
                                       GtkSelectionData *selection_data,
                                       guint             target_info,
                                       gpointer          user_data)
{
  ThunarClipboardManager *manager = THUNAR_CLIPBOARD_MANAGER (user_data);
  GList                  *file_list;
  gchar                  *str;
  gchar                 **uris;
  const gchar            *prefix;
  gsize                   len;

  _thunar_return_if_fail (GTK_IS_CLIPBOARD (clipboard));
  _thunar_return_if_fail (THUNAR_IS_CLIPBOARD_MANAGER (manager));
  _thunar_return_if_fail (manager->clipboard == clipboard);

  /* determine the path list from the file list */
  file_list = thunar_file_list_to_thunar_g_file_list (manager->files);

  switch (target_info)
    {
    case TARGET_TEXT_URI_LIST:
      uris = thunar_g_file_list_to_stringv (file_list);
      gtk_selection_data_set_uris (selection_data, uris);
      g_strfreev (uris);
      break;

    case TARGET_GNOME_COPIED_FILES:
      prefix = manager->files_cutted ? "cut\n" : "copy\n";
      str = thunar_clipboard_manager_g_file_list_to_string (file_list, prefix, FALSE, &len);
      gtk_selection_data_set (selection_data, gtk_selection_data_get_target (selection_data), 8, (guchar *) str, len);
      g_free (str);
      break;

    case TARGET_UTF8_STRING:
      str = thunar_clipboard_manager_g_file_list_to_string (file_list, NULL, TRUE, &len);
      gtk_selection_data_set_text (selection_data, str, len);
      g_free (str);
      break;

    default:
      _thunar_assert_not_reached ();
    }

  /* cleanup */
  thunar_g_list_free_full (file_list);
}



static void
thunar_clipboard_manager_clear_callback (GtkClipboard *clipboard,
                                         gpointer      user_data)
{
  ThunarClipboardManager *manager = THUNAR_CLIPBOARD_MANAGER (user_data);

  _thunar_return_if_fail (GTK_IS_CLIPBOARD (clipboard));
  _thunar_return_if_fail (THUNAR_IS_CLIPBOARD_MANAGER (manager));
  _thunar_return_if_fail (manager->clipboard == clipboard);

  /* release the pending files */
  thunar_clipboard_manager_release_files (manager);
}



static void
thunar_clipboard_manager_release_files (ThunarClipboardManager *manager)
{
  GList *lp;

  for (lp = manager->files; lp != NULL; lp = lp->next)
    {
      g_signal_handlers_disconnect_by_func (G_OBJECT (lp->data), thunar_clipboard_manager_file_destroyed, manager);
      g_object_unref (G_OBJECT (lp->data));
    }
  g_list_free (manager->files);
  manager->files = NULL;
}



#ifdef __APPLE__
/* re-reads the pasteboard state if it changed (or @force) */
static void
thunar_clipboard_manager_macos_update (ThunarClipboardManager *manager,
                                       gboolean                force)
{
  glong change_count = thunar_macos_pasteboard_change_count ();

  if (!force && change_count == manager->pb_change_count)
    return;
  manager->pb_change_count = change_count;

  /* someone else owns the pasteboard now, forget the cut files */
  if (change_count != manager->pb_owned_count)
    thunar_clipboard_manager_release_files (manager);

  g_free (manager->pb_image_mime_type);
  manager->pb_image_mime_type = thunar_macos_pasteboard_image_mime_type ();
  manager->can_paste = thunar_macos_pasteboard_has_files () || manager->pb_image_mime_type != NULL;

  /* notify listeners that we have a new clipboard state */
  g_signal_emit (manager, manager_signals[CHANGED], 0);
  g_object_notify (G_OBJECT (manager), "can-paste");
}



static gboolean
thunar_clipboard_manager_macos_poll (gpointer user_data)
{
  thunar_clipboard_manager_macos_update (THUNAR_CLIPBOARD_MANAGER (user_data), FALSE);
  return G_SOURCE_CONTINUE;
}
#endif



static void
thunar_clipboard_manager_transfer_files (ThunarClipboardManager *manager,
                                         gboolean                copy,
                                         GList                  *files)
{
  ThunarFile *file;
  GList      *lp;

  /* release any pending files */
  thunar_clipboard_manager_release_files (manager);

  /* remember the transfer operation */
  manager->files_cutted = !copy;

  /* setup the new file list */
  for (lp = g_list_last (files), manager->files = NULL; lp != NULL; lp = lp->prev)
    {
      file = THUNAR_FILE (g_object_ref (G_OBJECT (lp->data)));
      manager->files = g_list_prepend (manager->files, file);
      g_signal_connect (G_OBJECT (file), "destroy", G_CALLBACK (thunar_clipboard_manager_file_destroyed), manager);
    }

#ifdef __APPLE__
  {
    GList *file_list = thunar_file_list_to_thunar_g_file_list (manager->files);
    manager->pb_owned_count = thunar_macos_pasteboard_write_files (file_list, manager->files_cutted);
    thunar_g_list_free_full (file_list);
    thunar_clipboard_manager_macos_update (manager, TRUE);
    return;
  }
#endif

  /* acquire the CLIPBOARD ownership */
  gtk_clipboard_set_with_owner (manager->clipboard, clipboard_targets,
                                G_N_ELEMENTS (clipboard_targets),
                                thunar_clipboard_manager_get_callback,
                                thunar_clipboard_manager_clear_callback,
                                G_OBJECT (manager));

  /* Need to fake a "owner-change" event here if the Xserver doesn't support clipboard notification */
  if (!gdk_display_supports_selection_notification (gtk_clipboard_get_display (manager->clipboard)))
    thunar_clipboard_manager_owner_changed (manager->clipboard, NULL, manager);
}



/**
 * thunar_clipboard_manager_get_for_display:
 * @display : a #GdkDisplay.
 *
 * Determines the #ThunarClipboardManager that is used to manage
 * the clipboard on the given @display.
 *
 * The caller is responsible for freeing the returned object
 * using g_object_unref() when it's no longer needed.
 *
 * Return value: the #ThunarClipboardManager for @display.
 **/
ThunarClipboardManager *
thunar_clipboard_manager_get_for_display (GdkDisplay *display)
{
  ThunarClipboardManager *manager;
  GtkClipboard           *clipboard;

  _thunar_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);

  /* generate the quark on-demand */
  if (G_UNLIKELY (thunar_clipboard_manager_quark == 0))
    thunar_clipboard_manager_quark = g_quark_from_static_string ("thunar-clipboard-manager");

  /* figure out the clipboard for the given display */
  clipboard = gtk_clipboard_get_for_display (display, GDK_SELECTION_CLIPBOARD);

  /* check if a clipboard manager exists */
  manager = g_object_get_qdata (G_OBJECT (clipboard), thunar_clipboard_manager_quark);
  if (G_LIKELY (manager != NULL))
    {
      g_object_ref (G_OBJECT (manager));
      return manager;
    }

  /* allocate a new manager */
  manager = g_object_new (THUNAR_TYPE_CLIPBOARD_MANAGER, NULL);
  manager->clipboard = GTK_CLIPBOARD (g_object_ref (G_OBJECT (clipboard)));
  g_object_set_qdata (G_OBJECT (clipboard), thunar_clipboard_manager_quark, manager);

#ifdef __APPLE__
  /* look for usable data on the pasteboard, and whenever it changes */
  manager->pb_owned_count = -1;
  thunar_clipboard_manager_macos_update (manager, TRUE);
  manager->pb_poll_id = g_timeout_add (500, thunar_clipboard_manager_macos_poll, manager);
  return manager;
#endif

  /* listen for the "owner-change" signal on the clipboard */
  g_signal_connect (G_OBJECT (manager->clipboard), "owner-change",
                    G_CALLBACK (thunar_clipboard_manager_owner_changed), manager);

  /* look for usable data on the clipboard */
  thunar_clipboard_manager_owner_changed (manager->clipboard, NULL, manager);

  return manager;
}



/**
 * thunar_clipboard_manager_get_can_paste:
 * @manager : a #ThunarClipboardManager.
 *
 * Tells whether the contents of the clipboard represented
 * by @manager can be pasted into a folder.
 *
 * Return value: %TRUE if the contents of the clipboard
 *               represented by @manager can be pasted
 *               into a folder.
 **/
gboolean
thunar_clipboard_manager_get_can_paste (ThunarClipboardManager *manager)
{
  _thunar_return_val_if_fail (THUNAR_IS_CLIPBOARD_MANAGER (manager), FALSE);
#ifdef __APPLE__
  /* don't wait for the next poll, e.g. right after copying in Finder */
  thunar_clipboard_manager_macos_update (manager, FALSE);
#endif
  return manager->can_paste;
}



/**
 * thunar_clipboard_manager_has_cutted_file:
 * @manager : a #ThunarClipboardManager.
 * @file    : a #ThunarFile.
 *
 * Checks whether @file was cutted to the given @manager earlier.
 *
 * Return value: %TRUE if @file is on the cutted list of @manager.
 **/
gboolean
thunar_clipboard_manager_has_cutted_file (ThunarClipboardManager *manager,
                                          const ThunarFile       *file)
{
  _thunar_return_val_if_fail (THUNAR_IS_CLIPBOARD_MANAGER (manager), FALSE);
  _thunar_return_val_if_fail (THUNAR_IS_FILE (file), FALSE);

  return (manager->files_cutted && g_list_find (manager->files, file) != NULL);
}



/**
 * thunar_clipboard_manager_copy_files:
 * @manager : a #ThunarClipboardManager.
 * @files   : a list of #ThunarFile<!---->s.
 *
 * Sets the clipboard represented by @manager to
 * contain the @files and marks them to be copied
 * when the user pastes from the clipboard.
 **/
void
thunar_clipboard_manager_copy_files (ThunarClipboardManager *manager,
                                     GList                  *files)
{
  _thunar_return_if_fail (THUNAR_IS_CLIPBOARD_MANAGER (manager));
  thunar_clipboard_manager_transfer_files (manager, TRUE, files);
}



/**
 * thunar_clipboard_manager_cut_files:
 * @manager : a #ThunarClipboardManager.
 * @files   : a list of #ThunarFile<!---->s.
 *
 * Sets the clipboard represented by @manager to
 * contain the @files and marks them to be moved
 * when the user pastes from the clipboard.
 **/
void
thunar_clipboard_manager_cut_files (ThunarClipboardManager *manager,
                                    GList                  *files)
{
  _thunar_return_if_fail (THUNAR_IS_CLIPBOARD_MANAGER (manager));
  thunar_clipboard_manager_transfer_files (manager, FALSE, files);
}



/**
 * thunar_clipboard_manager_paste_files:
 * @manager           : a #ThunarClipboardManager.
 * @target_file       : the #GFile of the folder to which the contents on the clipboard
 *                      should be pasted.
 * @widget            : a #GtkWidget, on which to perform the paste or %NULL if no widget is
 *                      known.
 * @new_files_closure : a #GClosure to connect to the job's "new-files" signal,
 *                      which will be emitted when the job finishes with the
 *                      list of #GFile<!---->s created by the job, or
 *                      %NULL if you're not interested in the signal.
 * @as_links          : specifies whether to paste the files as symbolic links rather
 *                      than moving/copying the actual files
 *
 * Pastes the contents from the clipboard associated with @manager to the directory
 * referenced by @target_file.
 **/
void
thunar_clipboard_manager_paste_files (ThunarClipboardManager *manager,
                                      GFile                  *target_file,
                                      GtkWidget              *widget,
                                      GClosure               *new_files_closure,
                                      gboolean                as_links)
{
  ThunarClipboardPasteRequest *request;

  _thunar_return_if_fail (THUNAR_IS_CLIPBOARD_MANAGER (manager));
  _thunar_return_if_fail (widget == NULL || GTK_IS_WIDGET (widget));

  /* prepare the paste request */
  request = g_slice_new0 (ThunarClipboardPasteRequest);
  request->manager = THUNAR_CLIPBOARD_MANAGER (g_object_ref (G_OBJECT (manager)));
  request->target_file = g_object_ref (target_file);
  request->widget = widget;
  request->paste_as_link = as_links;

  /* take a reference on the closure (if any) */
  if (G_LIKELY (new_files_closure != NULL))
    {
      request->new_files_closure = new_files_closure;
      g_closure_ref (new_files_closure);
      g_closure_sink (new_files_closure);
    }

  /* get notified when the widget is destroyed prior to
   * completing the clipboard contents retrieval
   */
  if (G_LIKELY (request->widget != NULL))
    g_object_add_weak_pointer (G_OBJECT (request->widget), (gpointer) &request->widget);

  /* schedule the request */

#ifdef __APPLE__
  {
    GList   *file_list;
    gboolean cut = FALSE;

    thunar_clipboard_manager_macos_update (manager, FALSE);

    file_list = thunar_macos_pasteboard_read_files (&cut);
    if (file_list == NULL && manager->pb_image_mime_type != NULL)
      {
        GBytes *bytes = thunar_macos_pasteboard_read_image (manager->pb_image_mime_type);
        if (bytes != NULL)
          {
            gsize         length;
            gconstpointer content = g_bytes_get_data (bytes, &length);
            thunar_clipboard_manager_save_image (request, manager->pb_image_mime_type, content, length);
            g_bytes_unref (bytes);
          }
      }
    else if (thunar_clipboard_manager_paste_file_list (request, file_list, !cut) && cut)
      {
        /* cut files can only be pasted once */
        thunar_macos_pasteboard_clear (manager->pb_owned_count);
        thunar_clipboard_manager_macos_update (manager, FALSE);
      }

    thunar_g_list_free_full (file_list);
    thunar_clipboard_paste_request_free (request);
    return;
  }
#endif

  if (manager->image_target != NULL)
    {
      /* not using request_image because we want to save the image as-is, not do anything to it with pixbuf */
      gtk_clipboard_request_contents (manager->clipboard, manager->image_target, thunar_clipboard_manager_image_received, request);
    }
  else
    {
      gtk_clipboard_request_contents (manager->clipboard, manager->x_special_gnome_copied_files,
                                      thunar_clipboard_manager_contents_received, request);
    }
}
