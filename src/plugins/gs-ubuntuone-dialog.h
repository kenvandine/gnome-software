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

#ifndef GS_UBUNTUONE_DIALOG_H
#define GS_UBUNTUONE_DIALOG_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GS_TYPE_UBUNTUONE_DIALOG gs_ubuntuone_dialog_get_type ()

G_DECLARE_FINAL_TYPE (GsUbuntuoneDialog, gs_ubuntuone_dialog, GS, UBUNTUONE_DIALOG, GtkDialog)

GtkWidget	*gs_ubuntuone_dialog_new			(gboolean	    get_macaroon);
gboolean	 gs_ubuntuone_dialog_get_do_remember		(GsUbuntuoneDialog *dialog);
GVariant	*gs_ubuntuone_dialog_get_macaroon		(GsUbuntuoneDialog *dialog);
const gchar	*gs_ubuntuone_dialog_get_consumer_key		(GsUbuntuoneDialog *dialog);
const gchar	*gs_ubuntuone_dialog_get_consumer_secret	(GsUbuntuoneDialog *dialog);
const gchar	*gs_ubuntuone_dialog_get_token_key		(GsUbuntuoneDialog *dialog);
const gchar	*gs_ubuntuone_dialog_get_token_secret		(GsUbuntuoneDialog *dialog);

G_END_DECLS

#endif /* GS_UBUNTUONE_DIALOG_H */

/* vim: set noexpandtab: */
