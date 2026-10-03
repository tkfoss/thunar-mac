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

#ifndef __THUNAR_APP_INFO_H__
#define __THUNAR_APP_INFO_H__

/* Application registry used by Thunar. On Unix these are plain GIO calls;
 * on macOS they go through ThunarMacosAppInfo (LaunchServices + .desktop
 * command apps), because GOSXAppInfo can't list all apps, create apps from
 * a command line or remember the last used application. */

#include <gio/gio.h>

#ifdef __APPLE__

#include "thunar/thunar-macos-app-info.h"

#define thunar_app_info_create_from_commandline(cmd, name, flags, error) \
  thunar_macos_app_info_create_from_commandline ((cmd), (name), (error))
#define thunar_app_info_get_all thunar_macos_app_info_get_all
#define thunar_app_info_get_all_for_type thunar_macos_app_info_get_all_for_type
#define thunar_app_info_get_recommended_for_type thunar_macos_app_info_get_recommended_for_type
#define thunar_app_info_get_default_for_type thunar_macos_app_info_get_default_for_type
#define thunar_app_info_set_as_default_for_type thunar_macos_app_info_set_as_default_for_type
#define thunar_app_info_set_as_last_used_for_type thunar_macos_app_info_set_as_last_used_for_type

#else

#define thunar_app_info_create_from_commandline g_app_info_create_from_commandline
#define thunar_app_info_get_all g_app_info_get_all
#define thunar_app_info_get_all_for_type g_app_info_get_all_for_type
#define thunar_app_info_get_recommended_for_type g_app_info_get_recommended_for_type
#define thunar_app_info_get_default_for_type g_app_info_get_default_for_type
#define thunar_app_info_set_as_default_for_type g_app_info_set_as_default_for_type
#define thunar_app_info_set_as_last_used_for_type g_app_info_set_as_last_used_for_type

#endif

#endif /* !__THUNAR_APP_INFO_H__ */
