/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Kalev Lember <klember@redhat.com>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef __GS_OS_RELEASE_H
#define __GS_OS_RELEASE_H

#include <glib.h>

G_BEGIN_DECLS

#define GS_OS_RELEASE_ERROR gs_os_release_error_quark ()

typedef enum
{
	GS_OS_RELEASE_ERROR_FAILED
} GsOsReleaseError;

GQuark		  gs_os_release_error_quark			(void);
gchar		 *gs_os_release_get_name		(GError		**error);
gchar		 *gs_os_release_get_version		(GError		**error);
gchar		 *gs_os_release_get_id			(GError		**error);
gchar		 *gs_os_release_get_version_id		(GError		**error);
gchar		 *gs_os_release_get_pretty_name		(GError		**error);
gchar		 *gs_os_release_get			(const gchar	*name,
							 GError		**error);

G_END_DECLS

#endif /* __GS_OS_RELEASE_H */

/* vim: set noexpandtab: */
