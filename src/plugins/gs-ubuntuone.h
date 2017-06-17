/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Canonical Ltd.
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef __GS_UBUNTUONE_H
#define __GS_UBUNTUONE_H

#include <glib.h>

G_BEGIN_DECLS

gboolean	 gs_ubuntuone_get_macaroon	(gboolean	  use_cache,
						 gboolean	  show_dialog,
						 gchar		**macaroon,
						 gchar	       ***discharges,
						 GError		**error);

void		 gs_ubuntuone_clear_macaroon	(void);

gboolean	 gs_ubuntuone_get_credentials	(gchar	**consumer_key,
						 gchar	**consumer_secret,
						 gchar	**token_key,
						 gchar	**token_secret);

gboolean	 gs_ubuntuone_sign_in	(gchar	**consumer_key,
					 gchar	**consumer_secret,
					 gchar	**token_key,
					 gchar	**token_secret,
					 GError	**error);

G_END_DECLS

#endif /* __GS_UBUNTUONE_H */

