/*
 * Copyright (c) 2026 Thunar macOS port contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef __TSE_MACOS_H__
#define __TSE_MACOS_H__

#include <glib.h>

G_BEGIN_DECLS

/* Opens a new email with @attachments (NULL-terminated file:// URIs) in
 * the system mail client. Blocks until the mail client accepted them. */
gboolean
tse_macos_compose_email (gchar  **attachments,
                         GError **error);

G_END_DECLS

#endif /* !__TSE_MACOS_H__ */
