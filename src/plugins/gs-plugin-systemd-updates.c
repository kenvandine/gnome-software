/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013 Richard Hughes <richard@hughsie.com>
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

#include <config.h>
#include <gio/gio.h>

#define I_KNOW_THE_PACKAGEKIT_GLIB2_API_IS_SUBJECT_TO_CHANGE
#include <packagekit-glib2/packagekit.h>

#include <gs-plugin.h>

/*
 * SECTION:
 * Add previously downloads apps to the update list and also allow
 * scheduling the offline update.
 */

struct GsPluginPrivate {
	GFileMonitor		*monitor;
};

/**
 * gs_plugin_get_name:
 */
const gchar *
gs_plugin_get_name (void)
{
	return "systemd-updates";
}

/**
 * gs_plugin_initialize:
 */
void
gs_plugin_initialize (GsPlugin *plugin)
{
	plugin->priv = GS_PLUGIN_GET_PRIVATE (GsPluginPrivate);
}

/**
 * gs_plugin_destroy:
 */
void
gs_plugin_destroy (GsPlugin *plugin)
{
	if (plugin->priv->monitor != NULL)
		g_object_unref (plugin->priv->monitor);
}

/**
 * gs_plugin_systemd_updates_changed_cb:
 */
static void
gs_plugin_systemd_updates_changed_cb (GFileMonitor *monitor,
				      GFile *file, GFile *other_file,
				      GFileMonitorEvent event_type,
				      gpointer user_data)
{
	GsPlugin *plugin = GS_PLUGIN (user_data);
	gs_plugin_updates_changed (plugin);
}

/**
 * gs_plugin_setup:
 */
gboolean
gs_plugin_setup (GsPlugin *plugin, GCancellable *cancellable, GError **error)
{
	plugin->priv->monitor = pk_offline_get_prepared_monitor (cancellable, error);
	if (plugin->priv->monitor == NULL)
		return FALSE;
	g_signal_connect (plugin->priv->monitor, "changed",
			  G_CALLBACK (gs_plugin_systemd_updates_changed_cb),
			  plugin);
	return TRUE;
}

/**
 * gs_plugin_add_updates:
 */
gboolean
gs_plugin_add_updates (GsPlugin *plugin,
		       GList **list,
		       GCancellable *cancellable,
		       GError **error)
{
	guint i;
	g_autoptr(GError) error_local = NULL;
	g_auto(GStrv) package_ids = NULL;

	/* get the id's if the file exists */
	package_ids = pk_offline_get_prepared_ids (&error_local);
	if (package_ids == NULL) {
		if (g_error_matches (error_local,
				     PK_OFFLINE_ERROR,
				     PK_OFFLINE_ERROR_NO_DATA)) {
			return TRUE;
		}
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Failed to get prepared IDs: %s",
			     error_local->message);
		return FALSE;
	}

	/* add them to the new array */
	for (i = 0; package_ids[i] != NULL; i++) {
		g_autoptr(GsApp) app = NULL;
		g_auto(GStrv) split = NULL;
		app = gs_app_new (NULL);
		gs_app_set_management_plugin (app, "packagekit");
		gs_app_add_source_id (app, package_ids[i]);
		split = pk_package_id_split (package_ids[i]);
		gs_app_add_source (app, split[PK_PACKAGE_ID_NAME]);
		gs_app_set_update_version (app, split[PK_PACKAGE_ID_VERSION]);
		gs_app_set_state (app, AS_APP_STATE_UPDATABLE);
		gs_app_set_kind (app, AS_APP_KIND_GENERIC);
		gs_plugin_add_app (list, app);
	}
	return TRUE;
}

static gboolean
gs_plugin_systemd_updates_requires_trigger (GsApp *app)
{
	GPtrArray *related;
	guint i;

	/* look at related apps too */
	related = gs_app_get_related (app);
	for (i = 0; i < related->len; i++) {
		GsApp *app_tmp = g_ptr_array_index (related, i);
		if (gs_plugin_systemd_updates_requires_trigger (app_tmp))
			return TRUE;
	}

	/* if we can process this online do not require a trigger */
	if (gs_app_get_state (app) != AS_APP_STATE_UPDATABLE)
		return FALSE;

	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "packagekit") != 0)
		return FALSE;

	/* success! */
	return TRUE;
}

/**
 * gs_plugin_update:
 */
gboolean
gs_plugin_update (GsPlugin *plugin,
		  GList *apps,
		  GCancellable *cancellable,
		  GError **error)
{
	GList *l;

	/* any apps to process offline */
	for (l = apps; l != NULL; l = l->next) {
		GsApp *app = GS_APP (l->data);
		if (gs_plugin_systemd_updates_requires_trigger (app)) {
			return pk_offline_trigger (PK_OFFLINE_ACTION_REBOOT,
						   cancellable, error);
		}
	}
	return TRUE;
}

/**
 * gs_plugin_update_cancel:
 */
gboolean
gs_plugin_update_cancel (GsPlugin *plugin,
			 GsApp *app,
			 GCancellable *cancellable,
			 GError **error)
{
	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "packagekit") != 0)
		return TRUE;
	return pk_offline_cancel (NULL, error);
}

/**
 * gs_plugin_app_upgrade_trigger:
 */
gboolean
gs_plugin_app_upgrade_trigger (GsPlugin *plugin,
                               GsApp *app,
                               GCancellable *cancellable,
                               GError **error)
{
	/* only process this app if was created by this plugin */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "packagekit") != 0)
		return TRUE;
	return pk_offline_trigger_upgrade (PK_OFFLINE_ACTION_REBOOT, cancellable, error);
}
