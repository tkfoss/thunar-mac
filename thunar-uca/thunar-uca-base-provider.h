/* vi:set et ai sw=2 sts=2 ts=2: */
/*-
 * Copyright (c) 2005 Benedikt Meurer <benny@xfce.org>
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

#ifndef __THUNAR_UCA_BASE_PROVIDER_H__
#define __THUNAR_UCA_BASE_PROVIDER_H__

#include "thunar-uca/thunar-uca-model.h"

#include <thunarx/thunarx.h>

G_BEGIN_DECLS;

typedef struct _ThunarUcaBaseProviderClass ThunarUcaBaseProviderClass;
typedef struct _ThunarUcaBaseProvider      ThunarUcaBaseProvider;

#define THUNAR_UCA_TYPE_BASE_PROVIDER (thunar_uca_base_provider_get_type ())
#define THUNAR_UCA_BASE_PROVIDER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), THUNAR_UCA_TYPE_BASE_PROVIDER, ThunarUcaBaseProvider))
#define THUNAR_UCA_BASE_PROVIDER_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), THUNAR_UCA_TYPE_BASE_PROVIDER, ThunarUcaBaseProviderClass))
#define THUNAR_UCA_IS_BASE_PROVIDER(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), THUNAR_UCA_TYPE_BASE_PROVIDER))
#define THUNAR_UCA_IS_BASE_PROVIDER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), THUNAR_UCA_TYPE_BASE_PROVIDER))
#define THUNAR_UCA_BASE_PROVIDER_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), THUNAR_UCA_TYPE_BASE_PROVIDER, ThunarUcaBaseProviderClass))

struct _ThunarUcaBaseProviderClass
{
  GObjectClass __parent__;
};

struct _ThunarUcaBaseProvider
{
  GObject __parent__;

  ThunarUcaModel *model;
  gint            last_action_id; /* used to generate unique action names */

  /* child watch support for the last spawned child process
   * to be able to refresh the folder contents after the
   * child process has terminated.
   */
  gchar    *child_watch_path;
  GClosure *child_watch;
};

GType
thunar_uca_base_provider_get_type (void);
void
thunar_uca_base_provider_register_type (ThunarxProviderPlugin *plugin);

G_END_DECLS;

#endif /* !__THUNAR_UCA_BASE_PROVIDER_H__ */
