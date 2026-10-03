/* vi:set et ai sw=2 sts=2 ts=2: */
/*-
 * Copyright (c) 2005-2006 Benedikt Meurer <benny@xfce.org>
 * Copyright (c) 2026 The Xfce Development Team
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "thunar/thunar-uca-chooser.h"

#include "thunar-uca/thunar-uca-model.h"
#include "thunar/thunar-order-editor.h"
#include "thunar/thunar-private.h"
#include "thunar/thunar-uca-editor.h"

#include <libxfce4ui/libxfce4ui.h>



struct _ThunarUcaChooserClass
{
  ThunarOrderEditorClass __parent__;
};

struct _ThunarUcaChooser
{
  ThunarOrderEditor __parent__;

  ThunarUcaModel *uca_model;
};



static void
thunar_uca_chooser_finalize (GObject *object);



THUNARX_DEFINE_TYPE (ThunarUcaChooser, thunar_uca_chooser, THUNAR_TYPE_ORDER_EDITOR);



static void
thunar_uca_chooser_class_init (ThunarUcaChooserClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = thunar_uca_chooser_finalize;
}



static gboolean
thunar_uca_chooser_add (ThunarUcaChooser *chooser)
{
  gboolean status;

  /* if the write lock could not be obtained, cancel the action */
  if (!thunar_uca_editor_lock_model (GTK_WINDOW (chooser), chooser->uca_model))
    return TRUE;

  status = thunar_uca_editor_show (GTK_WINDOW (chooser), NULL, NULL, THUNAR_UCA_EDITOR_SHOW_FLAG_NONE);

  /* unlocking the model */
  thunar_uca_model_unlock (chooser->uca_model);

  if (status)
    {
      /* item added, continue event execution */
      return FALSE;
    }

  /* failed to add element, stop event processing */
  return TRUE;
}



static gboolean
thunar_uca_chooser_remove (ThunarUcaChooser *chooser,
                           gint             *indexes,
                           gint              n_items)
{
  if (thunar_uca_editor_ask_delete_items (GTK_WINDOW (chooser), chooser->uca_model))
    {
      /* Removing items in reverse order prevents index invalidation. XfceItemListView guarantees that the indexes in
       * this callback are in ascending order. */
      for (gint i = n_items - 1; i >= 0; --i)
        xfce_item_list_model_remove (XFCE_ITEM_LIST_MODEL (chooser->uca_model), indexes[i]);

      /* sync the model to persistent storage */
      thunar_uca_editor_save_persistently (GTK_WINDOW (chooser), chooser->uca_model);

      /* unlocking the model */
      thunar_uca_model_unlock (chooser->uca_model);
    }

  /* prevent event propagation; we remove the items manually, as this requires an additional step with
   * thunar_uca_editor_save_persistently() */
  return TRUE;
}



static gboolean
thunar_uca_chooser_edit (ThunarUcaChooser *chooser,
                         gint              index)
{
  GValue   value = G_VALUE_INIT;
  gboolean status;

  /* if the write lock could not be obtained, cancel the action */
  if (!thunar_uca_editor_lock_model (GTK_WINDOW (chooser), chooser->uca_model))
    return TRUE;

  xfce_item_list_model_get_item_value (XFCE_ITEM_LIST_MODEL (chooser->uca_model), index, THUNAR_UCA_MODEL_COLUMN_UNIQUE_ID, &value);
  status = thunar_uca_editor_show (GTK_WINDOW (chooser), g_value_get_string (&value), NULL, THUNAR_UCA_EDITOR_SHOW_FLAG_NONE);
  g_value_reset (&value);

  /* unlocking the model */
  thunar_uca_model_unlock (chooser->uca_model);

  if (!status)
    {
      /* cancel event propagation; the user clicked cancel */
      return TRUE;
    }

  /* the item was modified; continue event execution */
  return FALSE;
}



static void
thunar_uca_chooser_help (ThunarUcaChooser *chooser)
{
  xfce_dialog_show_help (GTK_WINDOW (chooser),
                         "thunar",
                         "custom-actions",
                         NULL);
}



static void
thunar_uca_chooser_init (ThunarUcaChooser *chooser)
{
  GtkWidget         *label;
  GtkWidget         *tree_view;
  GtkTreeSelection  *selection;
  XfceItemListView  *item_view;
  GtkTreeViewColumn *column;
  GtkCellRenderer   *cell_renderer;
  GMenu             *menu;
  GMenuItem         *menu_item;
  GIcon             *icon;

  chooser->uca_model = thunar_uca_model_get_default ();

  /* dialog */
  label = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (label),
                        _("Configure custom actions that will appear in the file manager's context menus and/or toolbar.\n"
                           "These actions are applicable to folders or certain kinds of files.\n"
                           "\n"
                           "Use the context menu configuration dialog or the toolbar configuration dialog to change the order of the actions."));

  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_vexpand (label, FALSE);
  gtk_container_add (thunar_order_editor_get_description_area (THUNAR_ORDER_EDITOR (chooser)), label);
  gtk_widget_show (label);

  g_signal_connect (chooser, "help", G_CALLBACK (thunar_uca_chooser_help), NULL);

  g_object_set (chooser,
                "title", _("Custom Actions"),
                "help-enabled", TRUE,
                NULL);

  /* item view */
  item_view = thunar_order_editor_get_item_view (THUNAR_ORDER_EDITOR (chooser));
  xfce_item_list_view_set_label_visibility (item_view, FALSE);
  xfce_item_list_view_set_model (item_view, XFCE_ITEM_LIST_MODEL (chooser->uca_model));

  /* item view menu */
  menu = xfce_item_list_view_get_menu (item_view);

  menu_item = g_menu_item_new (_("Add custom action"), "xfce-item-list-view.add-item");
  g_menu_item_set_attribute (menu_item, XFCE_MENU_ATTRIBUTE_TOOLTIP, "s", _("Add user custom action"));
  icon = g_themed_icon_new ("application-add-symbolic");
  g_menu_item_set_icon (menu_item, icon);
  g_clear_object (&icon);
  g_menu_insert_item (menu, 0, menu_item);
  g_object_unref (menu_item);

  /* item view signals */
  g_signal_connect_swapped (item_view, "add-item", G_CALLBACK (thunar_uca_chooser_add), chooser);
  g_signal_connect_swapped (item_view, "edit-item", G_CALLBACK (thunar_uca_chooser_edit), chooser);
  g_signal_connect_swapped (item_view, "remove-items", G_CALLBACK (thunar_uca_chooser_remove), chooser);

  /* tree view */
  tree_view = xfce_item_list_view_get_tree_view (item_view);
  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (tree_view));
  gtk_tree_selection_set_mode (selection, GTK_SELECTION_MULTIPLE);

  column = gtk_tree_view_get_column (GTK_TREE_VIEW (tree_view), XFCE_ITEM_LIST_VIEW_COLUMN_ICON);
  cell_renderer = g_object_get_data (G_OBJECT (column), "renderer");
  g_object_set (cell_renderer,
                "stock-size", GTK_ICON_SIZE_DND,
                "xpad", 2,
                "ypad", 2,
                NULL);
}



static void
thunar_uca_chooser_finalize (GObject *object)
{
  ThunarUcaChooser *chooser = THUNAR_UCA_CHOOSER (object);

  g_object_unref (chooser->uca_model);

  G_OBJECT_CLASS (thunar_uca_chooser_parent_class)->finalize (object);
}



void
thunar_uca_chooser_show (GtkWindow *window)
{
  ThunarUcaChooser *chooser;

  g_return_if_fail (window != NULL);
  g_return_if_fail (GTK_IS_WINDOW (window));

  chooser = g_object_new (THUNAR_UCA_TYPE_CHOOSER, NULL);
  thunar_order_editor_show (THUNAR_ORDER_EDITOR (chooser), GTK_WIDGET (window));
}
