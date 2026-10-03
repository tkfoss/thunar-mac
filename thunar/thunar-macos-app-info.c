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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

#include "thunar/thunar-macos-app-info.h"
#include "thunar/thunar-macos.h"

#define ASSOC_DEFAULT "Default Applications"
#define ASSOC_ADDED "Added Associations"
#define ASSOC_REMOVED "Removed Associations"

#define ICON_SIZE 128 /* px, rendered once per app, GTK scales it down */



typedef enum
{
  APP_KIND_BUNDLE,  /* .app bundle, launched through LaunchServices */
  APP_KIND_COMMAND, /* .desktop key file, spawned with field code expansion */
} AppKind;

struct _ThunarMacosAppInfo
{
  GObject __parent__;

  AppKind  kind;
  gchar   *id;         /* bundle id (or path) / desktop file id */
  gchar   *name;
  gchar   *comment;
  gchar   *path;       /* bundle path / .desktop file path (may be NULL) */
  gchar   *executable;
  gchar   *exec;       /* Exec= (command apps) */
  gchar   *icon_name;  /* Icon= (command apps) */
  gchar  **mime_types; /* MimeType= (command apps) */
  gboolean terminal;
  gboolean no_display;
  GIcon   *icon;
};



static void
thunar_macos_app_info_iface_init (GAppInfoIface *iface);
static void
thunar_macos_app_info_finalize (GObject *object);



G_DEFINE_TYPE_WITH_CODE (ThunarMacosAppInfo, thunar_macos_app_info, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_APP_INFO, thunar_macos_app_info_iface_init))



static void
thunar_macos_app_info_class_init (ThunarMacosAppInfoClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = thunar_macos_app_info_finalize;
}



static void
thunar_macos_app_info_init (ThunarMacosAppInfo *info)
{
}



static void
thunar_macos_app_info_finalize (GObject *object)
{
  ThunarMacosAppInfo *info = THUNAR_MACOS_APP_INFO (object);

  g_free (info->id);
  g_free (info->name);
  g_free (info->comment);
  g_free (info->path);
  g_free (info->executable);
  g_free (info->exec);
  g_free (info->icon_name);
  g_strfreev (info->mime_types);
  g_clear_object (&info->icon);

  G_OBJECT_CLASS (thunar_macos_app_info_parent_class)->finalize (object);
}



/* ---------------------------------------------------------------------- */
/* associations file (~/.config/Thunar/macos-mimeapps.list)               */
/* ---------------------------------------------------------------------- */

static gchar *
assoc_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "Thunar", "macos-mimeapps.list", NULL);
}



static GKeyFile *
assoc_load (void)
{
  GKeyFile *key_file = g_key_file_new ();
  gchar    *path = assoc_path ();

  g_key_file_load_from_file (key_file, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
  g_free (path);
  return key_file;
}



static gboolean
assoc_save (GKeyFile *key_file,
            GError  **error)
{
  gchar   *path = assoc_path ();
  gchar   *dir = g_path_get_dirname (path);
  gboolean result;

  g_mkdir_with_parents (dir, 0700);
  result = g_key_file_save_to_file (key_file, path, error);
  g_free (dir);
  g_free (path);
  return result;
}



static gchar **
assoc_get (GKeyFile    *key_file,
           const gchar *group,
           const gchar *content_type)
{
  gchar **list = g_key_file_get_string_list (key_file, group, content_type, NULL, NULL);
  return list != NULL ? list : g_new0 (gchar *, 1);
}



#define ASSOC_PREPEND 0
#define ASSOC_APPEND (-1)
#define ASSOC_REMOVE (-2)

/* removes @id from the list in @group/@content_type and re-adds it at the
 * front (ASSOC_PREPEND) or end (ASSOC_APPEND), or not at all (ASSOC_REMOVE) */
static void
assoc_update (GKeyFile    *key_file,
              const gchar *group,
              const gchar *content_type,
              const gchar *id,
              gint         position)
{
  gchar    **list = assoc_get (key_file, group, content_type);
  GPtrArray *array = g_ptr_array_new ();
  guint      n;

  if (position == ASSOC_PREPEND)
    g_ptr_array_add (array, (gpointer) id);
  for (n = 0; list[n] != NULL; n++)
    if (g_strcmp0 (list[n], id) != 0)
      g_ptr_array_add (array, list[n]);
  if (position == ASSOC_APPEND)
    g_ptr_array_add (array, (gpointer) id);

  if (array->len > 0)
    g_key_file_set_string_list (key_file, group, content_type, (const gchar *const *) array->pdata, array->len);
  else
    g_key_file_remove_key (key_file, group, content_type, NULL);

  g_ptr_array_free (array, TRUE);
  g_strfreev (list);
}



/* ---------------------------------------------------------------------- */
/* app info construction                                                  */
/* ---------------------------------------------------------------------- */

static GHashTable *bundle_cache = NULL; /* path -> ThunarMacosAppInfo */



/**
 * thunar_macos_app_info_new_for_bundle:
 * @app_path : path of an .app bundle.
 *
 * Return value: (transfer full): a #GAppInfo or %NULL.
 **/
GAppInfo *
thunar_macos_app_info_new_for_bundle (const gchar *app_path)
{
  ThunarMacosAppInfo *info;
  gchar              *bundle_id = NULL;
  gchar              *name = NULL;
  gchar              *executable = NULL;

  if (app_path == NULL)
    return NULL;

  if (G_UNLIKELY (bundle_cache == NULL))
    bundle_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  info = g_hash_table_lookup (bundle_cache, app_path);
  if (info != NULL)
    return g_object_ref (G_APP_INFO (info));

  if (!thunar_macos_app_bundle_info (app_path, &bundle_id, &name, &executable))
    return NULL;

  info = g_object_new (THUNAR_TYPE_MACOS_APP_INFO, NULL);
  info->kind = APP_KIND_BUNDLE;
  info->path = g_strdup (app_path);
  info->id = bundle_id != NULL ? bundle_id : g_strdup (app_path);
  info->name = name != NULL ? name : g_path_get_basename (app_path);
  info->executable = executable;

  g_hash_table_insert (bundle_cache, g_strdup (app_path), g_object_ref (info));
  return G_APP_INFO (info);
}



/**
 * thunar_macos_app_info_new_from_keyfile:
 * @key_file : a .desktop key file.
 * @filename : the path of the key file or %NULL.
 *
 * Creates a command app info from a desktop entry (Name, Exec, Icon,
 * MimeType, Terminal, NoDisplay). Returns %NULL for hidden entries,
 * non-applications and when the program in Exec/TryExec is missing.
 *
 * Return value: (transfer full): a #GAppInfo or %NULL.
 **/
GAppInfo *
thunar_macos_app_info_new_from_keyfile (GKeyFile    *key_file,
                                        const gchar *filename)
{
  ThunarMacosAppInfo *info;
  gchar              *type;
  gchar              *exec;
  gchar              *try_exec;
  gchar             **argv = NULL;
  gchar              *program;

  if (!g_key_file_has_group (key_file, G_KEY_FILE_DESKTOP_GROUP))
    return NULL;

  type = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE, NULL);
  if (g_strcmp0 (type, G_KEY_FILE_DESKTOP_TYPE_APPLICATION) != 0
      || g_key_file_get_boolean (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_HIDDEN, NULL))
    {
      g_free (type);
      return NULL;
    }
  g_free (type);

  exec = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, NULL);
  if (exec == NULL || !g_shell_parse_argv (exec, NULL, &argv, NULL))
    {
      g_free (exec);
      return NULL;
    }

  /* skip entries for programs that are not installed */
  try_exec = g_key_file_get_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TRY_EXEC, NULL);
  program = g_find_program_in_path (try_exec != NULL ? try_exec : argv[0]);
  g_free (try_exec);
  if (program == NULL)
    {
      g_strfreev (argv);
      g_free (exec);
      return NULL;
    }

  info = g_object_new (THUNAR_TYPE_MACOS_APP_INFO, NULL);
  info->kind = APP_KIND_COMMAND;
  info->exec = exec;
  info->executable = g_strdup (argv[0]);
  info->path = g_strdup (filename);
  info->name = g_key_file_get_locale_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, NULL, NULL);
  info->comment = g_key_file_get_locale_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_COMMENT, NULL, NULL);
  info->icon_name = g_key_file_get_locale_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ICON, NULL, NULL);
  info->mime_types = g_key_file_get_string_list (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_MIME_TYPE, NULL, NULL);
  info->terminal = g_key_file_get_boolean (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TERMINAL, NULL);
  info->no_display = g_key_file_get_boolean (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NO_DISPLAY, NULL);
  info->id = filename != NULL ? g_path_get_basename (filename) : g_strdup_printf ("%s.desktop", argv[0]);
  if (info->name == NULL)
    info->name = g_path_get_basename (argv[0]);

  g_free (program);
  g_strfreev (argv);
  return G_APP_INFO (info);
}



const gchar *const *
thunar_macos_app_info_get_mime_types (ThunarMacosAppInfo *info)
{
  g_return_val_if_fail (THUNAR_IS_MACOS_APP_INFO (info), NULL);
  return (const gchar *const *) info->mime_types;
}



static gboolean
command_supports_uris (ThunarMacosAppInfo *info)
{
  return info->exec != NULL && (strstr (info->exec, "%u") != NULL || strstr (info->exec, "%U") != NULL);
}



static gboolean
command_supports_content_type (ThunarMacosAppInfo *info,
                               const gchar        *content_type)
{
  guint n;

  if (info->mime_types == NULL)
    return FALSE;

  /* g_content_type_is_mime_type() converts MIME -> UTI and checks conformance,
   * like GDesktopAppInfo also offers apps registered for parent types */
  for (n = 0; info->mime_types[n] != NULL; n++)
    if (*info->mime_types[n] != '\0' && g_content_type_is_mime_type (content_type, info->mime_types[n]))
      return TRUE;

  return FALSE;
}



/* ---------------------------------------------------------------------- */
/* .desktop applications ($XDG_DATA_HOME, $XDG_DATA_DIRS /applications)   */
/* ---------------------------------------------------------------------- */

static GList *desktop_apps = NULL;
static gchar *desktop_apps_stamp = NULL;



static gchar *
user_desktop_dir (void)
{
  return g_build_filename (g_get_user_data_dir (), "applications", NULL);
}



static gchar **
desktop_dirs (void)
{
  const gchar *const *system_dirs = g_get_system_data_dirs ();
  GPtrArray          *array = g_ptr_array_new ();
  guint               n;

  g_ptr_array_add (array, user_desktop_dir ());
  for (n = 0; system_dirs[n] != NULL; n++)
    g_ptr_array_add (array, g_build_filename (system_dirs[n], "applications", NULL));
  g_ptr_array_add (array, NULL);

  return (gchar **) g_ptr_array_free (array, FALSE);
}



/* string of the mtimes of all directories, to detect changes cheaply */
static gchar *
desktop_dirs_stamp (gchar **dirs)
{
  GString    *stamp = g_string_new (NULL);
  struct stat st;
  guint       n;

  for (n = 0; dirs[n] != NULL; n++)
    {
      if (stat (dirs[n], &st) == 0)
        g_string_append_printf (stamp, "%ld;", (long) st.st_mtime);
      else
        g_string_append (stamp, "-;");
    }

  return g_string_free (stamp, FALSE);
}



static void
desktop_apps_load_dir (const gchar *dir,
                       const gchar *prefix,
                       GHashTable  *seen)
{
  const gchar *name;
  GDir        *gdir;
  GKeyFile    *key_file;
  GAppInfo    *info;
  gchar       *path;
  gchar       *id;

  gdir = g_dir_open (dir, 0, NULL);
  if (gdir == NULL)
    return;

  while ((name = g_dir_read_name (gdir)) != NULL)
    {
      path = g_build_filename (dir, name, NULL);

      /* desktop file ids of subdirectories use '-' as separator */
      if (g_file_test (path, G_FILE_TEST_IS_DIR))
        {
          gchar *sub_prefix = g_strconcat (prefix, name, "-", NULL);
          desktop_apps_load_dir (path, sub_prefix, seen);
          g_free (sub_prefix);
        }
      else if (g_str_has_suffix (name, ".desktop"))
        {
          id = g_strconcat (prefix, name, NULL);
          if (!g_hash_table_contains (seen, id))
            {
              /* earlier directories (user dir first) override later ones,
               * also when the entry is hidden */
              g_hash_table_add (seen, g_strdup (id));

              key_file = g_key_file_new ();
              if (g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, NULL))
                {
                  info = thunar_macos_app_info_new_from_keyfile (key_file, path);
                  if (info != NULL)
                    {
                      g_free (THUNAR_MACOS_APP_INFO (info)->id);
                      THUNAR_MACOS_APP_INFO (info)->id = g_strdup (id);
                      desktop_apps = g_list_prepend (desktop_apps, info);
                    }
                }
              g_key_file_free (key_file);
            }
          g_free (id);
        }

      g_free (path);
    }

  g_dir_close (gdir);
}



/* the cached list of command apps (owned by the cache) */
static GList *
desktop_apps_get (void)
{
  gchar     **dirs = desktop_dirs ();
  gchar      *stamp = desktop_dirs_stamp (dirs);
  GHashTable *seen;
  guint       n;

  if (g_strcmp0 (stamp, desktop_apps_stamp) != 0)
    {
      g_list_free_full (desktop_apps, g_object_unref);
      desktop_apps = NULL;

      seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      for (n = 0; dirs[n] != NULL; n++)
        desktop_apps_load_dir (dirs[n], "", seen);
      g_hash_table_destroy (seen);

      desktop_apps = g_list_reverse (desktop_apps);
      g_free (desktop_apps_stamp);
      desktop_apps_stamp = stamp;
    }
  else
    {
      g_free (stamp);
    }

  g_strfreev (dirs);
  return desktop_apps;
}



/* resolves an id from the associations file (desktop id or bundle id/path) */
static GAppInfo *
app_info_lookup (const gchar *id)
{
  GAppInfo *info = NULL;
  GList    *lp;
  gchar    *path;

  if (id == NULL || *id == '\0')
    return NULL;

  if (g_str_has_suffix (id, ".desktop"))
    {
      for (lp = desktop_apps_get (); lp != NULL; lp = lp->next)
        if (g_strcmp0 (THUNAR_MACOS_APP_INFO (lp->data)->id, id) == 0)
          return g_object_ref (lp->data);
      return NULL;
    }

  if (g_path_is_absolute (id))
    return g_file_test (id, G_FILE_TEST_IS_DIR) ? thunar_macos_app_info_new_for_bundle (id) : NULL;

  path = thunar_macos_app_path_for_bundle_id (id);
  if (path != NULL)
    info = thunar_macos_app_info_new_for_bundle (path);
  g_free (path);

  return info;
}



/* ---------------------------------------------------------------------- */
/* launching                                                              */
/* ---------------------------------------------------------------------- */

static void
launch_finished (GError  *error,
                 gpointer user_data)
{
  gchar     *name = user_data;
  GtkWidget *dialog;

  if (error != NULL)
    {
      dialog = gtk_message_dialog_new (NULL, 0, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                       _("Failed to launch \"%s\""), name);
      gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s", error->message);
      g_signal_connect (dialog, "response", G_CALLBACK (gtk_widget_destroy), NULL);
      gtk_widget_show (dialog);
    }

  g_free (name);
}



/* file:// URI for files with a local path (also GVfs-less schemes like
 * our trash:// that still map to a path), the native URI otherwise */
static gchar *
file_to_uri (GFile *file)
{
  gchar *path = g_file_get_path (file);
  gchar *uri;

  if (path != NULL)
    {
      uri = g_filename_to_uri (path, NULL, NULL);
      g_free (path);
      if (uri != NULL)
        return uri;
    }

  return g_file_get_uri (file);
}



static gchar *
file_to_path (GFile *file)
{
  gchar *path = g_file_get_path (file);
  return path != NULL ? path : g_file_get_uri (file);
}



static gboolean
bundle_launch (ThunarMacosAppInfo *info,
               GList              *files,
               GError            **error)
{
  GPtrArray *uris = g_ptr_array_new_with_free_func (g_free);
  GList     *lp;
  gboolean   result;

  for (lp = files; lp != NULL; lp = lp->next)
    g_ptr_array_add (uris, file_to_uri (lp->data));
  g_ptr_array_add (uris, NULL);

  result = thunar_macos_open_with_app (info->path, (const gchar *const *) uris->pdata,
                                       launch_finished, g_strdup (info->name));
  g_ptr_array_free (uris, TRUE);

  if (!result)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Failed to launch \"%s\""), info->name);

  return result;
}



/* expands the field codes of the Exec key for one invocation (desktop entry
 * spec), advancing @files past the files used */
static gchar **
command_expand (ThunarMacosAppInfo *info,
                GList             **files,
                gboolean           *uses_files,
                GError            **error)
{
  GPtrArray *array;
  GString   *arg;
  gchar    **argv;
  gboolean   consumed_one = FALSE;
  gboolean   consumed_all = FALSE;
  guint      n;
  gchar     *p;
  gchar     *s;
  GList     *lp;

  if (!g_shell_parse_argv (info->exec, NULL, &argv, error))
    return NULL;

  *uses_files = FALSE;
  array = g_ptr_array_new_with_free_func (g_free);

  for (n = 0; argv[n] != NULL; n++)
    {
      /* %F / %U as a separate argument expand to all files */
      if (strcmp (argv[n], "%F") == 0 || strcmp (argv[n], "%U") == 0)
        {
          *uses_files = TRUE;
          consumed_all = TRUE;
          for (lp = *files; lp != NULL; lp = lp->next)
            g_ptr_array_add (array, argv[n][1] == 'F' ? file_to_path (lp->data) : file_to_uri (lp->data));
          continue;
        }

      /* %i expands to "--icon <Icon>" or nothing */
      if (strcmp (argv[n], "%i") == 0)
        {
          if (info->icon_name != NULL && *info->icon_name != '\0')
            {
              g_ptr_array_add (array, g_strdup ("--icon"));
              g_ptr_array_add (array, g_strdup (info->icon_name));
            }
          continue;
        }

      arg = g_string_new (NULL);
      for (p = argv[n]; *p != '\0'; p++)
        {
          if (*p != '%' || p[1] == '\0')
            {
              g_string_append_c (arg, *p);
              continue;
            }

          switch (*++p)
            {
            case 'f':
            case 'F':
            case 'u':
            case 'U':
              /* %F/%U inside a longer argument: first file only */
              *uses_files = TRUE;
              if (*files != NULL)
                {
                  s = (*p == 'f' || *p == 'F') ? file_to_path ((*files)->data) : file_to_uri ((*files)->data);
                  g_string_append (arg, s);
                  g_free (s);
                  consumed_one = TRUE;
                }
              break;

            case 'c':
              g_string_append (arg, info->name);
              break;

            case 'k':
              if (info->path != NULL)
                g_string_append (arg, info->path);
              break;

            case '%':
              g_string_append_c (arg, '%');
              break;

            default:
              /* deprecated (%d, %D, %n, %N, %v, %m) or unknown: dropped */
              break;
            }
        }

      /* an argument that was only a field code without value is dropped */
      if (arg->len > 0 || argv[n][0] != '%')
        g_ptr_array_add (array, g_string_free (arg, FALSE));
      else
        g_string_free (arg, TRUE);
    }

  g_strfreev (argv);

  if (consumed_all)
    *files = NULL;
  else if (consumed_one)
    *files = (*files)->next;

  if (array->len == 0)
    {
      g_ptr_array_free (array, TRUE);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("Empty command"));
      return NULL;
    }

  g_ptr_array_add (array, NULL);
  return (gchar **) g_ptr_array_free (array, FALSE);
}



/* escapes @str for an AppleScript string literal */
static gchar *
applescript_escape (const gchar *str)
{
  GString *s = g_string_new (NULL);

  for (; *str != '\0'; str++)
    {
      if (*str == '"' || *str == '\\')
        g_string_append_c (s, '\\');
      g_string_append_c (s, *str);
    }

  return g_string_free (s, FALSE);
}



static gboolean
command_spawn (ThunarMacosAppInfo *info,
               gchar             **argv,
               GAppLaunchContext  *context,
               GError            **error)
{
  gchar  **envp = NULL;
  gchar  **term_argv = NULL;
  gboolean result;

  if (context != NULL)
    envp = g_app_launch_context_get_environment (context);

  if (info->terminal)
    {
      /* Terminal=true: run the command in a new Terminal.app window */
      GString *cmd = g_string_new (NULL);
      gchar   *escaped;
      guint    n;

      for (n = 0; argv[n] != NULL; n++)
        {
          gchar *quoted = g_shell_quote (argv[n]);
          if (n > 0)
            g_string_append_c (cmd, ' ');
          g_string_append (cmd, quoted);
          g_free (quoted);
        }

      escaped = applescript_escape (cmd->str);
      term_argv = g_new0 (gchar *, 6);
      term_argv[0] = g_strdup ("/usr/bin/osascript");
      term_argv[1] = g_strdup ("-e");
      term_argv[2] = g_strdup_printf ("tell application \"Terminal\" to do script \"%s\"", escaped);
      term_argv[3] = g_strdup ("-e");
      term_argv[4] = g_strdup ("tell application \"Terminal\" to activate");
      g_string_free (cmd, TRUE);
      g_free (escaped);
      argv = term_argv;
    }

  result = g_spawn_async (NULL, argv, envp, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, error);

  g_strfreev (term_argv);
  g_strfreev (envp);
  return result;
}



static gboolean
command_launch (ThunarMacosAppInfo *info,
                GList              *files,
                GAppLaunchContext  *context,
                GError            **error)
{
  gboolean uses_files;
  gchar  **argv;

  do
    {
      argv = command_expand (info, &files, &uses_files, error);
      if (argv == NULL)
        return FALSE;

      if (!command_spawn (info, argv, context, error))
        {
          g_strfreev (argv);
          return FALSE;
        }
      g_strfreev (argv);
    }
  /* one invocation per file for %f / %u */
  while (uses_files && files != NULL);

  return TRUE;
}



/* ---------------------------------------------------------------------- */
/* GAppInfo interface                                                     */
/* ---------------------------------------------------------------------- */

static GAppInfo *
app_info_dup (GAppInfo *appinfo)
{
  ThunarMacosAppInfo *info = THUNAR_MACOS_APP_INFO (appinfo);
  ThunarMacosAppInfo *copy = g_object_new (THUNAR_TYPE_MACOS_APP_INFO, NULL);

  copy->kind = info->kind;
  copy->id = g_strdup (info->id);
  copy->name = g_strdup (info->name);
  copy->comment = g_strdup (info->comment);
  copy->path = g_strdup (info->path);
  copy->executable = g_strdup (info->executable);
  copy->exec = g_strdup (info->exec);
  copy->icon_name = g_strdup (info->icon_name);
  copy->mime_types = g_strdupv (info->mime_types);
  copy->terminal = info->terminal;
  copy->no_display = info->no_display;
  copy->icon = info->icon != NULL ? g_object_ref (info->icon) : NULL;

  return G_APP_INFO (copy);
}



static gboolean
app_info_equal (GAppInfo *appinfo1,
                GAppInfo *appinfo2)
{
  ThunarMacosAppInfo *a = THUNAR_MACOS_APP_INFO (appinfo1);
  ThunarMacosAppInfo *b = THUNAR_MACOS_APP_INFO (appinfo2);

  return a->kind == b->kind && g_strcmp0 (a->id, b->id) == 0;
}



static const char *
app_info_get_id (GAppInfo *appinfo)
{
  return THUNAR_MACOS_APP_INFO (appinfo)->id;
}



static const char *
app_info_get_name (GAppInfo *appinfo)
{
  return THUNAR_MACOS_APP_INFO (appinfo)->name;
}



static const char *
app_info_get_description (GAppInfo *appinfo)
{
  return THUNAR_MACOS_APP_INFO (appinfo)->comment;
}



static const char *
app_info_get_executable (GAppInfo *appinfo)
{
  return THUNAR_MACOS_APP_INFO (appinfo)->executable;
}



static const char *
app_info_get_commandline (GAppInfo *appinfo)
{
  return THUNAR_MACOS_APP_INFO (appinfo)->exec;
}



static GIcon *
app_info_get_icon (GAppInfo *appinfo)
{
  ThunarMacosAppInfo *info = THUNAR_MACOS_APP_INFO (appinfo);
  GBytes             *bytes;
  GFile              *file;
  gchar              *name;
  gchar              *dot;

  if (info->icon != NULL)
    return info->icon;

  if (info->kind == APP_KIND_BUNDLE)
    {
      /* the Finder icon, rendered lazily since this is not cheap */
      bytes = thunar_macos_file_icon_png (info->path, ICON_SIZE);
      if (bytes != NULL)
        {
          info->icon = g_bytes_icon_new (bytes);
          g_bytes_unref (bytes);
        }
    }
  else if (info->icon_name != NULL && *info->icon_name != '\0')
    {
      if (g_path_is_absolute (info->icon_name))
        {
          file = g_file_new_for_path (info->icon_name);
          info->icon = g_file_icon_new (file);
          g_object_unref (file);
        }
      else
        {
          /* strip extensions some desktop files wrongly use */
          name = g_strdup (info->icon_name);
          dot = strrchr (name, '.');
          if (dot != NULL && (strcmp (dot, ".png") == 0 || strcmp (dot, ".svg") == 0 || strcmp (dot, ".xpm") == 0))
            *dot = '\0';
          info->icon = g_themed_icon_new_with_default_fallbacks (name);
          g_free (name);
        }
    }

  if (info->icon == NULL)
    info->icon = g_themed_icon_new ("application-x-executable");

  return info->icon;
}



static gboolean
app_info_launch (GAppInfo          *appinfo,
                 GList             *files,
                 GAppLaunchContext *context,
                 GError           **error)
{
  ThunarMacosAppInfo *info = THUNAR_MACOS_APP_INFO (appinfo);

  if (info->kind == APP_KIND_BUNDLE)
    return bundle_launch (info, files, error);
  else
    return command_launch (info, files, context, error);
}



static gboolean
app_info_supports_uris (GAppInfo *appinfo)
{
  ThunarMacosAppInfo *info = THUNAR_MACOS_APP_INFO (appinfo);
  return info->kind == APP_KIND_BUNDLE || command_supports_uris (info);
}



static gboolean
app_info_supports_files (GAppInfo *appinfo)
{
  return TRUE;
}



static gboolean
app_info_launch_uris (GAppInfo          *appinfo,
                      GList             *uris,
                      GAppLaunchContext *context,
                      GError           **error)
{
  GList   *files = NULL;
  GList   *lp;
  gboolean result;

  for (lp = uris; lp != NULL; lp = lp->next)
    files = g_list_prepend (files, g_file_new_for_uri (lp->data));
  files = g_list_reverse (files);

  result = app_info_launch (appinfo, files, context, error);
  g_list_free_full (files, g_object_unref);
  return result;
}



static gboolean
app_info_should_show (GAppInfo *appinfo)
{
  return !THUNAR_MACOS_APP_INFO (appinfo)->no_display;
}



static gboolean
app_info_set_as_default_for_type (GAppInfo    *appinfo,
                                  const char  *content_type,
                                  GError     **error)
{
  return thunar_macos_app_info_set_as_default_for_type (appinfo, content_type, error);
}



static gboolean
app_info_set_as_last_used_for_type (GAppInfo    *appinfo,
                                    const char  *content_type,
                                    GError     **error)
{
  return thunar_macos_app_info_set_as_last_used_for_type (appinfo, content_type, error);
}



static gboolean
app_info_add_supports_type (GAppInfo    *appinfo,
                            const char  *content_type,
                            GError     **error)
{
  GKeyFile *key_file = assoc_load ();
  gboolean  result;

  assoc_update (key_file, ASSOC_ADDED, content_type, THUNAR_MACOS_APP_INFO (appinfo)->id, ASSOC_APPEND);
  assoc_update (key_file, ASSOC_REMOVED, content_type, THUNAR_MACOS_APP_INFO (appinfo)->id, ASSOC_REMOVE);
  result = assoc_save (key_file, error);
  g_key_file_free (key_file);
  return result;
}



static gboolean
app_info_can_remove_supports_type (GAppInfo *appinfo)
{
  return TRUE;
}



static gboolean
app_info_remove_supports_type (GAppInfo    *appinfo,
                               const char  *content_type,
                               GError     **error)
{
  const gchar *id = THUNAR_MACOS_APP_INFO (appinfo)->id;
  GKeyFile    *key_file = assoc_load ();
  gchar       *default_id;
  gboolean     result;

  /* LaunchServices registrations can't be removed, hide them instead */
  assoc_update (key_file, ASSOC_ADDED, content_type, id, ASSOC_REMOVE);
  assoc_update (key_file, ASSOC_REMOVED, content_type, id, ASSOC_APPEND);

  default_id = g_key_file_get_string (key_file, ASSOC_DEFAULT, content_type, NULL);
  if (g_strcmp0 (default_id, id) == 0)
    g_key_file_remove_key (key_file, ASSOC_DEFAULT, content_type, NULL);
  g_free (default_id);

  result = assoc_save (key_file, error);
  g_key_file_free (key_file);
  return result;
}



static const char **
app_info_get_supported_types (GAppInfo *appinfo)
{
  return (const char **) THUNAR_MACOS_APP_INFO (appinfo)->mime_types;
}



static gboolean
app_info_can_delete (GAppInfo *appinfo)
{
  ThunarMacosAppInfo *info = THUNAR_MACOS_APP_INFO (appinfo);
  gchar              *dir;
  gboolean            result;

  /* only command apps the user created (custom commands) */
  if (info->kind != APP_KIND_COMMAND || info->path == NULL)
    return FALSE;

  dir = user_desktop_dir ();
  result = g_str_has_prefix (info->path, dir) && g_access (info->path, W_OK) == 0;
  g_free (dir);
  return result;
}



static gboolean
app_info_delete (GAppInfo *appinfo)
{
  ThunarMacosAppInfo *info = THUNAR_MACOS_APP_INFO (appinfo);

  GKeyFile           *key_file;
  gchar             **groups;
  gchar             **keys;
  gchar              *value;
  guint               n, m;

  if (!app_info_can_delete (appinfo))
    return FALSE;

  if (g_unlink (info->path) != 0)
    return FALSE;

  /* forget all associations of the deleted app */
  key_file = assoc_load ();
  groups = g_key_file_get_groups (key_file, NULL);
  for (n = 0; groups[n] != NULL; n++)
    {
      keys = g_key_file_get_keys (key_file, groups[n], NULL, NULL);
      for (m = 0; keys != NULL && keys[m] != NULL; m++)
        {
          if (strcmp (groups[n], ASSOC_DEFAULT) == 0)
            {
              value = g_key_file_get_string (key_file, groups[n], keys[m], NULL);
              if (g_strcmp0 (value, info->id) == 0)
                g_key_file_remove_key (key_file, groups[n], keys[m], NULL);
              g_free (value);
            }
          else
            assoc_update (key_file, groups[n], keys[m], info->id, ASSOC_REMOVE);
        }
      g_strfreev (keys);
    }
  g_strfreev (groups);
  assoc_save (key_file, NULL);
  g_key_file_free (key_file);

  return TRUE;
}



static const char *
app_info_get_display_name (GAppInfo *appinfo)
{
  return THUNAR_MACOS_APP_INFO (appinfo)->name;
}



static void
thunar_macos_app_info_iface_init (GAppInfoIface *iface)
{
  iface->dup = app_info_dup;
  iface->equal = app_info_equal;
  iface->get_id = app_info_get_id;
  iface->get_name = app_info_get_name;
  iface->get_description = app_info_get_description;
  iface->get_executable = app_info_get_executable;
  iface->get_icon = app_info_get_icon;
  iface->launch = app_info_launch;
  iface->supports_uris = app_info_supports_uris;
  iface->supports_files = app_info_supports_files;
  iface->launch_uris = app_info_launch_uris;
  iface->should_show = app_info_should_show;
  iface->set_as_default_for_type = app_info_set_as_default_for_type;
  iface->add_supports_type = app_info_add_supports_type;
  iface->can_remove_supports_type = app_info_can_remove_supports_type;
  iface->remove_supports_type = app_info_remove_supports_type;
  iface->can_delete = app_info_can_delete;
  iface->do_delete = app_info_delete;
  iface->get_commandline = app_info_get_commandline;
  iface->get_display_name = app_info_get_display_name;
  iface->set_as_last_used_for_type = app_info_set_as_last_used_for_type;
  iface->get_supported_types = app_info_get_supported_types;
}



/* ---------------------------------------------------------------------- */
/* registry                                                               */
/* ---------------------------------------------------------------------- */

/* appends @info to @list unless an equal app is in it or @removed, takes @info */
static GList *
app_list_add (GList        *list,
              GAppInfo     *info,
              gchar *const *removed)
{
  GList *lp;

  if (info == NULL)
    return list;

  if (removed != NULL && g_strv_contains ((const gchar *const *) removed, g_app_info_get_id (info)))
    {
      g_object_unref (info);
      return list;
    }

  for (lp = list; lp != NULL; lp = lp->next)
    if (g_app_info_equal (lp->data, info))
      {
        g_object_unref (info);
        return list;
      }

  return g_list_append (list, info);
}



/**
 * thunar_macos_app_info_create_from_commandline:
 *
 * Replacement for g_app_info_create_from_commandline() (not supported by
 * GLib on macOS). Like GLib on Unix this writes a .desktop file to
 * $XDG_DATA_HOME/applications, so the command shows up again later; an
 * existing custom command with the same command line is reused.
 **/
GAppInfo *
thunar_macos_app_info_create_from_commandline (const gchar *commandline,
                                               const gchar *application_name,
                                               GError     **error)
{
  GKeyFile *key_file;
  GAppInfo *info = NULL;
  GList    *lp;
  gchar    *exec;
  gchar    *dir;
  gchar    *path;
  gchar    *data;
  gchar    *basename;
  gchar    *p;
  gchar   **argv;
  gsize     length;
  gint      fd;

  g_return_val_if_fail (commandline != NULL, NULL);

  /* validate the command line */
  if (!g_shell_parse_argv (commandline, NULL, &argv, error))
    return NULL;
  basename = g_path_get_basename (argv[0]);
  g_strfreev (argv);

  /* without field codes, pass the files at the end (like GLib does) */
  if (strstr (commandline, "%f") == NULL && strstr (commandline, "%F") == NULL
      && strstr (commandline, "%u") == NULL && strstr (commandline, "%U") == NULL)
    exec = g_strconcat (commandline, " %F", NULL);
  else
    exec = g_strdup (commandline);

  /* reuse an existing custom command */
  for (lp = desktop_apps_get (); lp != NULL; lp = lp->next)
    if (g_strcmp0 (THUNAR_MACOS_APP_INFO (lp->data)->exec, exec) == 0
        && app_info_can_delete (lp->data))
      {
        g_free (exec);
        g_free (basename);
        return g_object_ref (lp->data);
      }

  key_file = g_key_file_new ();
  g_key_file_set_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE, G_KEY_FILE_DESKTOP_TYPE_APPLICATION);
  g_key_file_set_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME,
                         application_name != NULL ? application_name : basename);
  g_key_file_set_string (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, exec);
  g_key_file_set_boolean (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TERMINAL, FALSE);
  g_key_file_set_boolean (key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NO_DISPLAY, TRUE);
  data = g_key_file_to_data (key_file, &length, NULL);
  g_key_file_free (key_file);

  /* sanitize the name for the file name */
  for (p = basename; *p != '\0'; p++)
    if (!g_ascii_isalnum (*p) && *p != '-' && *p != '_')
      *p = '_';

  dir = user_desktop_dir ();
  g_mkdir_with_parents (dir, 0700);
  path = g_strdup_printf ("%s/thunar-userapp-%s-XXXXXX.desktop", dir, basename);
  fd = g_mkstemp_full (path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "%s", g_strerror (errno));
    }
  else
    {
      close (fd);
      if (g_file_set_contents (path, data, length, error))
        {
          /* reload and return the entry from the cache (proper desktop id) */
          gchar *id = g_path_get_basename (path);
          g_free (desktop_apps_stamp);
          desktop_apps_stamp = NULL;
          info = app_info_lookup (id);
          g_free (id);

          if (info == NULL)
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                         _("Command \"%s\" not found"), commandline);
        }
    }

  g_free (path);
  g_free (dir);
  g_free (data);
  g_free (exec);
  g_free (basename);
  return info;
}



/* all bundle apps, rescanned at most every 30 seconds */
static GList *
bundle_apps_get (void)
{
  static GList  *apps = NULL;
  static gint64  stamp = 0;
  gchar        **paths;
  guint          n;

  if (apps != NULL && g_get_monotonic_time () - stamp < 30 * G_USEC_PER_SEC)
    return apps;

  g_list_free_full (apps, g_object_unref);
  apps = NULL;

  paths = thunar_macos_all_apps ();
  for (n = 0; paths[n] != NULL; n++)
    apps = app_list_add (apps, thunar_macos_app_info_new_for_bundle (paths[n]), NULL);
  g_strfreev (paths);

  stamp = g_get_monotonic_time ();
  return apps;
}



GList *
thunar_macos_app_info_get_all (void)
{
  GList *result = NULL;
  GList *lp;

  for (lp = bundle_apps_get (); lp != NULL; lp = lp->next)
    result = g_list_prepend (result, g_object_ref (lp->data));
  for (lp = desktop_apps_get (); lp != NULL; lp = lp->next)
    result = g_list_prepend (result, g_object_ref (lp->data));

  return g_list_reverse (result);
}



/* @recommended_only: only the apps the user added/used and the default */
static GList *
get_for_type (const gchar *content_type,
              gboolean     recommended_only)
{
  GKeyFile *key_file;
  GList    *result = NULL;
  GList    *lp;
  gchar   **removed;
  gchar   **added;
  gchar   **paths;
  gchar    *path;
  guint     n;

  if (content_type == NULL)
    return NULL;

  key_file = assoc_load ();
  removed = assoc_get (key_file, ASSOC_REMOVED, content_type);
  added = assoc_get (key_file, ASSOC_ADDED, content_type);

  /* last used / added first */
  for (n = 0; added[n] != NULL; n++)
    result = app_list_add (result, app_info_lookup (added[n]), removed);

  if (recommended_only)
    {
      path = thunar_macos_default_app_for_content_type (content_type);
      result = app_list_add (result, thunar_macos_app_info_new_for_bundle (path), removed);
      g_free (path);
    }
  else
    {
      /* .desktop apps with a matching MimeType */
      for (lp = desktop_apps_get (); lp != NULL; lp = lp->next)
        if (command_supports_content_type (lp->data, content_type))
          result = app_list_add (result, g_object_ref (lp->data), removed);

      /* LaunchServices (default first) */
      paths = thunar_macos_apps_for_content_type (content_type);
      for (n = 0; paths[n] != NULL; n++)
        result = app_list_add (result, thunar_macos_app_info_new_for_bundle (paths[n]), removed);
      g_strfreev (paths);
    }

  g_strfreev (added);
  g_strfreev (removed);
  g_key_file_free (key_file);
  return result;
}



GList *
thunar_macos_app_info_get_all_for_type (const gchar *content_type)
{
  return get_for_type (content_type, FALSE);
}



GList *
thunar_macos_app_info_get_recommended_for_type (const gchar *content_type)
{
  return get_for_type (content_type, TRUE);
}



GAppInfo *
thunar_macos_app_info_get_default_for_type (const gchar *content_type,
                                            gboolean     must_support_uris)
{
  GKeyFile *key_file;
  GAppInfo *info = NULL;
  gchar    *id;
  gchar    *path;
  gchar   **added;
  guint     n;

  if (content_type == NULL)
    return NULL;

  key_file = assoc_load ();

  /* a command app the user chose as default */
  id = g_key_file_get_string (key_file, ASSOC_DEFAULT, content_type, NULL);
  info = app_info_lookup (id);
  g_free (id);
  if (info != NULL && must_support_uris && !g_app_info_supports_uris (info))
    g_clear_object (&info);

  /* the LaunchServices default */
  if (info == NULL)
    {
      path = thunar_macos_default_app_for_content_type (content_type);
      info = thunar_macos_app_info_new_for_bundle (path);
      g_free (path);
    }

  /* no default: fall back to the last used app, like GIO on Unix */
  if (info == NULL)
    {
      added = assoc_get (key_file, ASSOC_ADDED, content_type);
      for (n = 0; added[n] != NULL && info == NULL; n++)
        {
          info = app_info_lookup (added[n]);
          if (info != NULL && must_support_uris && !g_app_info_supports_uris (info))
            g_clear_object (&info);
        }
      g_strfreev (added);
    }

  g_key_file_free (key_file);
  return info;
}



gboolean
thunar_macos_app_info_set_as_default_for_type (GAppInfo    *info,
                                               const gchar *content_type,
                                               GError     **error)
{
  ThunarMacosAppInfo *mac_info;
  GKeyFile           *key_file;
  gboolean            result;

  g_return_val_if_fail (G_IS_APP_INFO (info), FALSE);

  if (!THUNAR_IS_MACOS_APP_INFO (info))
    return g_app_info_set_as_default_for_type (info, content_type, error);

  mac_info = THUNAR_MACOS_APP_INFO (info);
  key_file = assoc_load ();

  if (mac_info->kind == APP_KIND_BUNDLE)
    {
      /* system wide, like Finder's "Change All..." */
      if (!thunar_macos_set_default_app_for_content_type (mac_info->path, content_type, error))
        {
          g_key_file_free (key_file);
          return FALSE;
        }
      g_key_file_remove_key (key_file, ASSOC_DEFAULT, content_type, NULL);
    }
  else
    {
      /* LaunchServices only knows bundles, remember command apps ourselves */
      g_key_file_set_string (key_file, ASSOC_DEFAULT, content_type, mac_info->id);
    }

  assoc_update (key_file, ASSOC_ADDED, content_type, mac_info->id, ASSOC_PREPEND);
  assoc_update (key_file, ASSOC_REMOVED, content_type, mac_info->id, ASSOC_REMOVE);
  result = assoc_save (key_file, error);
  g_key_file_free (key_file);
  return result;
}



gboolean
thunar_macos_app_info_set_as_last_used_for_type (GAppInfo    *info,
                                                 const gchar *content_type,
                                                 GError     **error)
{
  GKeyFile *key_file;
  gboolean  result;

  g_return_val_if_fail (G_IS_APP_INFO (info), FALSE);

  if (!THUNAR_IS_MACOS_APP_INFO (info))
    return g_app_info_set_as_last_used_for_type (info, content_type, error);

  key_file = assoc_load ();
  assoc_update (key_file, ASSOC_ADDED, content_type, g_app_info_get_id (info), ASSOC_PREPEND);
  assoc_update (key_file, ASSOC_REMOVED, content_type, g_app_info_get_id (info), ASSOC_REMOVE);
  result = assoc_save (key_file, error);
  g_key_file_free (key_file);
  return result;
}
