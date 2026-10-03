/* vi:set et ai sw=2 sts=2 ts=2: */
/*-
 * Copyright (c) 2005-2006 Benedikt Meurer <benny@xfce.org>
 * Copyright (c) 2009 Jannis Pohlmann <jannis@xfce.org>
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

#include "thunar/thunar-uca-provider.h"

#include "thunar-uca/thunar-uca-base-provider.h"
#include "thunar-uca/thunar-uca-context.h"
#include "thunar-uca/thunar-uca-model.h"
#include "thunar/thunar-action-manager.h"
#include "thunar/thunar-uca-chooser.h"

#include <gio/gio.h>
#include <libxfce4ui/libxfce4ui.h>
#include <libxfce4util/libxfce4util.h>



static void
thunar_uca_provider_preferences_provider_init (ThunarxPreferencesProviderIface *iface);
static GList *
thunar_uca_provider_get_menu_items (ThunarxPreferencesProvider *preferences_provider,
                                    GtkWidget                  *window);



struct _ThunarUcaProviderClass
{
  ThunarUcaBaseProviderClass __parent__;
};

struct _ThunarUcaProvider
{
  ThunarUcaBaseProvider __parent__;
};



THUNARX_DEFINE_TYPE_WITH_CODE (ThunarUcaProvider,
                               thunar_uca_provider,
                               THUNAR_UCA_TYPE_BASE_PROVIDER,
                               THUNARX_IMPLEMENT_INTERFACE (THUNARX_TYPE_PREFERENCES_PROVIDER,
                                                            thunar_uca_provider_preferences_provider_init));



static void
thunar_uca_provider_class_init (ThunarUcaProviderClass *klass)
{
}



static void
thunar_uca_provider_preferences_provider_init (ThunarxPreferencesProviderIface *iface)
{
  iface->get_menu_items = thunar_uca_provider_get_menu_items;
}



static void
thunar_uca_provider_init (ThunarUcaProvider *uca_provider)
{
}



static void
manage_menu_items (GtkWindow *window)
{
  thunar_uca_chooser_show (window);
}



static GList *
thunar_uca_provider_get_menu_items (ThunarxPreferencesProvider *preferences_provider,
                                    GtkWidget                  *window)
{
  ThunarxMenuItem *item;
  GClosure        *closure;

  item = thunarx_menu_item_new ("ThunarUca::manage-actions", _("Confi_gure custom actions..."), _("Setup custom actions that will appear in the file managers context menus"), NULL);
  closure = g_cclosure_new_object_swap (G_CALLBACK (manage_menu_items), G_OBJECT (window));
  g_signal_connect_closure (G_OBJECT (item), "activate", closure, TRUE);

  return g_list_prepend (NULL, item);
}
