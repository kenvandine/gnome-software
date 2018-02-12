/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015 Canonical Ltd
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

#include <gs-utils.h>
#include <gs-plugin.h>

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include "gs-snapd.h"
#include "gs-ubuntuone.h"

struct GsPluginPrivate {
	gboolean	 system_is_confined;
	gchar		*store_name;
	GMutex		 store_snaps_lock;
	GHashTable	*store_snaps;
};

typedef gboolean (*AppFilterFunc)(const gchar *id, JsonObject *object, gpointer data);

const gchar *
gs_plugin_get_name (void)
{
	return "snap";
}

const gchar **
gs_plugin_order_after (GsPlugin *plugin)
{
	static const gchar *deps[] = {
		"appstream",		/* Override hardcoded popular apps */
		"hardcoded-featured",	/* Override hardcoded popular apps */
		NULL };
	return deps;
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
	g_autoptr(JsonObject) system_information = NULL;

	/* create private area */
	plugin->priv = GS_PLUGIN_GET_PRIVATE (GsPluginPrivate);

	system_information = gs_snapd_get_system_info (NULL, NULL);
	if (system_information == NULL) {
		g_debug ("disabling '%s' as snapd not running",
			 gs_plugin_get_name ());
		gs_plugin_set_enabled (plugin, FALSE);
	}
	plugin->priv->system_is_confined = g_strcmp0 (json_object_get_string_member (system_information, "confinement"), "strict") == 0;
	if (json_object_has_member (system_information, "store"))
		plugin->priv->store_name = g_strdup (json_object_get_string_member (system_information, "store"));
	else
		plugin->priv->store_name = g_strdup (/* TRANSLATORS: default snap store name */
						     _("Snap Store"));

	g_mutex_init (&plugin->priv->store_snaps_lock);
	plugin->priv->store_snaps = g_hash_table_new_full (g_str_hash, g_str_equal,
							   g_free, (GDestroyNotify) json_object_unref);
}

static gboolean
gs_plugin_snap_set_app_pixbuf_from_data (GsApp *app, const gchar *buf, gsize count, GError **error)
{
	g_autoptr(GdkPixbufLoader) loader = NULL;
	g_autoptr(GError) error_local = NULL;

	loader = gdk_pixbuf_loader_new ();
	if (!gdk_pixbuf_loader_write (loader, buf, count, &error_local)) {
		g_debug ("icon_data[%" G_GSIZE_FORMAT "]=%s", count, buf);
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Failed to write: %s",
			     error_local->message);
		return FALSE;
	}
	if (!gdk_pixbuf_loader_close (loader, &error_local)) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Failed to close: %s",
			     error_local->message);
		return FALSE;
	}
	gs_app_set_pixbuf (app, gdk_pixbuf_loader_get_pixbuf (loader));
	return TRUE;
}

static JsonObject *
store_snap_cache_lookup (GsPlugin *plugin, const gchar *name)
{
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&plugin->priv->store_snaps_lock);
	return g_hash_table_lookup (plugin->priv->store_snaps, name);
}

static void
store_snap_cache_update (GsPlugin *plugin, JsonArray *snaps)
{
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&plugin->priv->store_snaps_lock);
	guint i;

	for (i = 0; i < json_array_get_length (snaps); i++) {
		JsonObject *snap = json_array_get_object_element (snaps, i);
		g_hash_table_insert (plugin->priv->store_snaps, g_strdup (json_object_get_string_member (snap, "name")), json_object_ref (snap));
	}
}

static JsonArray *
find_snaps (GsPlugin *plugin, const gchar *section, gboolean match_name, const gchar *query, GCancellable *cancellable, GError **error)
{
	g_autoptr(JsonArray) snaps = NULL;

	snaps = gs_snapd_find (section, match_name, query, cancellable, error);
	if (snaps == NULL)
		return NULL;

	/* Only cache name results because they return channel information
	 * https://forum.snapcraft.io/t/channel-maps-list-is-empty-when-using-v1-snaps-search-as-opposed-to-using-v2-snaps-details */
	if (match_name)
		store_snap_cache_update (plugin, snaps);

	return g_steal_pointer (&snaps);
}

static const gchar *
get_snap_title (JsonObject *snap)
{
	const gchar *name = NULL;

	if (json_object_has_member (snap, "title"))
		name = json_object_get_string_member (snap, "title");
	if (name == NULL || g_strcmp0 (name, "") == 0)
		name = json_object_get_string_member (snap, "name");

	return name;
}

static GsApp *
snap_to_app (GsPlugin *plugin, JsonObject *snap)
{
	GsApp *app;
	const gchar *type, *confinement;

	/* create a unique ID for deduplication, TODO: branch? */
	app = gs_app_new (json_object_get_string_member (snap, "name"));
	type = json_object_get_string_member (snap, "type");
	if (g_strcmp0 (type, "app") == 0) {
		gs_app_set_kind (app, AS_APP_KIND_DESKTOP);
	} else if (g_strcmp0 (type, "gadget") == 0 || g_strcmp0 (type, "os") == 0) {
		gs_app_set_kind (app, AS_APP_KIND_RUNTIME);
		gs_app_add_quirk (app, AS_APP_QUIRK_NOT_LAUNCHABLE);
	}
	gs_app_set_management_plugin (app, "snap");
	gs_app_add_quirk (app, AS_APP_QUIRK_NOT_REVIEWABLE);
	gs_app_set_name (app, GS_APP_QUALITY_HIGHEST, get_snap_title (snap));
	if (gs_plugin_check_distro_id (plugin, "ubuntu"))
		gs_app_add_quirk (app, AS_APP_QUIRK_PROVENANCE);
	confinement = json_object_get_string_member (snap, "confinement");
	gs_app_set_metadata (app, "snap::confinement", confinement);
	if (plugin->priv->system_is_confined && g_strcmp0 (confinement, "strict") == 0)
		gs_app_add_kudo (app, GS_APP_KUDO_SANDBOXED);
	else
		gs_app_remove_kudo (app, GS_APP_KUDO_SANDBOXED);

	return app;
}

gboolean
gs_plugin_url_to_app (GsPlugin *plugin,
		      GList **list,
		      const gchar *url,
		      GCancellable *cancellable,
		      GError **error)
{
	g_autofree gchar *scheme = NULL;
	g_autoptr(JsonArray) snaps = NULL;
	JsonObject *snap;
	g_autofree gchar *path = NULL;
	g_autoptr(GsApp) app = NULL;
	g_autofree gchar *channel_name = NULL;

	/* not us */
	scheme = gs_utils_get_url_scheme (url);
	if (g_strcmp0 (scheme, "snap") != 0)
		return TRUE;

	/* create app */
	path = gs_utils_get_url_path (url);
	snaps = find_snaps (plugin, NULL, TRUE, path, cancellable, NULL);
	if (snaps == NULL || json_array_get_length (snaps) < 1)
		return TRUE;

	snap = json_array_get_object_element (snaps, 0);
	app = snap_to_app (plugin, snap);
	channel_name = gs_utils_get_url_query_param (url, "channel");
	if (channel_name != NULL)
		gs_app_set_metadata (app, "snap::channel", channel_name);
	gs_plugin_add_app (list, app);

	return TRUE;
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	g_free (plugin->priv->store_name);
	g_hash_table_unref (plugin->priv->store_snaps);
	g_mutex_clear (&plugin->priv->store_snaps_lock);
}

static gboolean
is_banner_image (const gchar *filename)
{
	/* Check if this screenshot was uploaded as "banner.png" or "banner.jpg".
	 * The server optionally adds a 7 character suffix onto it if it would collide with
	 * an existing name, e.g. "banner_MgEy4MI.png"
	 * See https://forum.snapcraft.io/t/improve-method-for-setting-featured-snap-banner-image-in-store/
	 */
	return g_regex_match_simple ("^banner(?:_[a-zA-Z0-9]{7})?\\.(?:png|jpg)$", filename, 0, 0);
}

static gboolean
is_banner_icon_image (const gchar *filename)
{
	/* Check if this screenshot was uploaded as "banner-icon.png" or "banner-icon.jpg".
	 * The server optionally adds a 7 character suffix onto it if it would collide with
	 * an existing name, e.g. "banner-icon_Ugn6pmj.png"
	 * See https://forum.snapcraft.io/t/improve-method-for-setting-featured-snap-banner-image-in-store/
	 */
	return g_regex_match_simple ("^banner-icon(?:_[a-zA-Z0-9]{7})?\\.(?:png|jpg)$", filename, 0, 0);
}

static gboolean
remove_cb (GsApp *app, gpointer user_data)
{
	return FALSE;
}

gboolean
gs_plugin_add_featured (GsPlugin *plugin,
		        GList **list,
		        GCancellable *cancellable,
		        GError **error)
{
	g_autoptr(JsonArray) snaps = NULL;
	JsonObject *snap;
	g_autoptr(GsApp) app = NULL;
	const gchar *banner_url = NULL, *icon_url = NULL;
	g_autoptr(GString) background_css = NULL;

	snaps = find_snaps (plugin, "featured", FALSE, NULL, cancellable, error);

	if (snaps == NULL)
		return FALSE;

	if (json_array_get_length (snaps) < 1)
		return TRUE;

	/* use first snap as the featured app */
	snap = json_array_get_object_element (snaps, 0);
	app = snap_to_app (plugin, snap);

	/* if has a sceenshot called 'banner.png' or 'banner-icon.png' then use them for the banner */
	if (json_object_has_member (snap, "screenshots")) {
		JsonArray *screenshots;
		guint i;

		screenshots = json_object_get_array_member (snap, "screenshots");
		for (i = 0; i < json_array_get_length (screenshots); i++) {
			JsonObject *screenshot = json_array_get_object_element (screenshots, i);
			const gchar *url;
			g_autofree gchar *filename = NULL;

			url = json_object_get_string_member (screenshot, "url");
			filename = g_path_get_basename (url);
			if (is_banner_image (filename))
				banner_url = url;
			else if (is_banner_icon_image (filename))
				icon_url = url;
		}
	}

	background_css = g_string_new ("");
	if (icon_url != NULL)
		g_string_append_printf (background_css,
					"url('%s') left center / auto 100%% no-repeat, ",
					icon_url);
	else
		g_string_append_printf (background_css,
					"url('%s') left center / auto 100%% no-repeat, ",
					json_object_get_string_member (snap, "icon"));
	if (banner_url != NULL)
		g_string_append_printf (background_css,
					"url('%s') center / cover no-repeat;",
					banner_url);
	else
		g_string_append_printf (background_css, "#FFFFFF;");
	gs_app_add_kudo (app, GS_APP_KUDO_FEATURED_RECOMMENDED);
	gs_app_set_metadata (app, "Featured::text-color", "#000000");
	gs_app_set_metadata (app, "Featured::background", background_css->str);
	gs_app_set_metadata (app, "Featured::stroke-color", "#000000");
	gs_app_set_metadata (app, "Featured::text-shadow", "0 1px 1px rgba(255,255,255,0.5)");

	/* replace any other featured apps with our one */
	gs_plugin_list_filter (list, remove_cb, NULL);
	gs_plugin_add_app (list, app);

	return TRUE;
}

gboolean
gs_plugin_add_popular (GsPlugin *plugin,
		       GList **list,
		       GCancellable *cancellable,
		       GError **error)
{
	g_autoptr(JsonArray) snaps = NULL;
	guint i;

	snaps = find_snaps (plugin, "featured", FALSE, NULL, cancellable, error);
	if (snaps == NULL)
		return FALSE;

	/* replace any other popular apps with our one */
	gs_plugin_list_filter (list, remove_cb, NULL);

	/* skip first snap - it is used as the featured app */
	for (i = 1; i < json_array_get_length (snaps); i++) {
		g_autoptr(GsApp) app = snap_to_app (plugin, json_array_get_object_element (snaps, i));
		gs_plugin_add_app (list, app);
	}

	return TRUE;
}

gboolean
gs_plugin_add_category_apps (GsPlugin *plugin,
			     GsCategory *category,
			     GList **list,
			     GCancellable *cancellable,
			     GError **error)
{
	GsCategory *c;
	g_autoptr(GString) id = NULL;
	const gchar *sections = NULL;

	id = g_string_new ("");
	for (c = category; c != NULL; c = gs_category_get_parent (c)) {
		if (c != category)
			g_string_prepend (id, "/");
		g_string_prepend (id, gs_category_get_id (c));
	}

	if (strcmp (id->str, "Game/all") == 0)
		sections = "games";
	else if (strcmp (id->str, "Audio/all") == 0)
		sections = "music";
	else if (strcmp (id->str, "Video/all") == 0)
		sections = "video";
	else if (strcmp (id->str, "Graphics/all") == 0)
		sections = "graphics";
	else if (strcmp (id->str, "Network/all") == 0)
		sections = "social-networking";
	else if (strcmp (id->str, "Office/all") == 0)
		sections = "productivity;finance";
	else if (strcmp (id->str, "Development/all") == 0)
		sections = "developers";
	else if (strcmp (id->str, "Utility/all") == 0)
		sections = "utilities";

	if (sections != NULL) {
		g_auto(GStrv) tokens = NULL;
		int i;

		tokens = g_strsplit (sections, ";", -1);
		for (i = 0; tokens[i] != NULL; i++) {
			g_autoptr(JsonArray) snaps = NULL;
			guint j;

			snaps = find_snaps (plugin, tokens[i], FALSE, NULL, cancellable, error);
			if (snaps == NULL)
				return FALSE;
			for (j = 0; j < json_array_get_length (snaps); j++) {
				g_autoptr(GsApp) app = snap_to_app (plugin, json_array_get_object_element (snaps, j));
				gs_plugin_add_app (list, app);
			}
		}
	}
	return TRUE;
}

gboolean
gs_plugin_add_installed (GsPlugin *plugin,
			 GList **list,
			 GCancellable *cancellable,
			 GError **error)
{
	g_autoptr(JsonArray) snaps = NULL;
	guint i;

	snaps = gs_snapd_list (cancellable, error);
	if (snaps == NULL)
		return FALSE;

	for (i = 0; i < json_array_get_length (snaps); i++) {
		JsonObject *snap = json_array_get_object_element (snaps, i);
		g_autoptr(GsApp) app = NULL;
		const gchar *status;

		status = json_object_get_string_member (snap, "status");
		if (g_strcmp0 (status, "active") != 0)
			continue;

		app = snap_to_app (plugin, snap);
		gs_plugin_add_app (list, app);
	}

	return TRUE;
}

gboolean
gs_plugin_add_search (GsPlugin *plugin,
		      gchar **values,
		      GList **list,
		      GCancellable *cancellable,
		      GError **error)
{
	g_autofree gchar *query = NULL;
	g_autoptr(JsonArray) snaps = NULL;
	guint i;

	query = g_strjoinv (" ", values);
	snaps = find_snaps (plugin, NULL, FALSE, query, cancellable, error);
	if (snaps == NULL)
		return FALSE;

	for (i = 0; i < json_array_get_length (snaps); i++) {
		g_autoptr(GsApp) app = snap_to_app (plugin, json_array_get_object_element (snaps, i));
		gs_plugin_add_app (list, app);
	}

	return TRUE;
}

static gboolean
load_icon (GsPlugin *plugin, GsApp *app, const gchar *icon_url, GCancellable *cancellable, GError **error)
{
	if (icon_url == NULL || g_strcmp0 (icon_url, "") == 0) {
		g_autoptr(AsIcon) icon = as_icon_new ();
		as_icon_set_kind (icon, AS_ICON_KIND_STOCK);
		as_icon_set_name (icon, "package-x-generic");
		gs_app_set_icon (app, icon);
		return TRUE;
	}

	/* icon is optional, either loaded from snapd or from a URL */
	if (g_str_has_prefix (icon_url, "/")) {
		g_autofree gchar *icon_data = NULL;
		gsize icon_data_length;

		icon_data = gs_snapd_get_resource (icon_url, &icon_data_length, cancellable, error);
		if (icon_data == NULL)
			return FALSE;

		if (!gs_plugin_snap_set_app_pixbuf_from_data (app,
							      icon_data, icon_data_length,
							      error)) {
			g_prefix_error (error, "Failed to load %s: ", icon_url);
			return FALSE;
		}
	} else {
		g_autofree gchar *basename_tmp = NULL;
		g_autofree gchar *hash = NULL;
		g_autofree gchar *basename = NULL;
		g_autofree gchar *cache_dir = NULL;
		g_autofree gchar *cache_fn = NULL;
		g_autoptr(SoupMessage) message = NULL;
		g_autoptr(GdkPixbufLoader) loader = NULL;
		g_autoptr(GError) local_error = NULL;
		g_autoptr(AsIcon) icon = NULL;

		icon = as_icon_new ();
		gs_app_set_icon (app, icon);

		/* attempt to load from cache */
		basename_tmp = g_path_get_basename (icon_url);
		hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, icon_url, -1);
		basename = g_strdup_printf ("%s-%s", hash, basename_tmp);
		cache_dir = gs_utils_get_cachedir ("snap-icons", error);
		cache_fn = g_build_filename (cache_dir, basename, NULL);
		as_icon_set_filename (icon, cache_fn);
		if (g_file_test (cache_fn, G_FILE_TEST_EXISTS)) {
			as_icon_set_kind (icon, AS_ICON_KIND_LOCAL);
			if (gs_app_load_icon (app, plugin->scale, &local_error))
				return TRUE;

			g_warning ("Failed to load cached icon: %s", local_error->message);
		}

		as_icon_set_kind (icon, AS_ICON_KIND_REMOTE);
		as_icon_set_url (icon, icon_url);
	}

	return TRUE;
}

static JsonObject *
get_store_snap (GsPlugin *plugin, const gchar *name, GCancellable *cancellable, GError **error)
{
	JsonObject *snap = NULL;
	g_autoptr(JsonArray) snaps = NULL;

	/* use cached version if available */
	snap = store_snap_cache_lookup (plugin, name);
	if (snap != NULL)
		return json_object_ref (snap);

	snaps = find_snaps (plugin, NULL, TRUE, name, cancellable, error);
	if (snaps == NULL || json_array_get_length (snaps) < 1)
		return NULL;

	return json_object_ref (json_array_get_object_element (snaps, 0));
}

static void
add_channel (GsApp *app, const gchar *name, const gchar *version, const gchar *tracking_channel)
{
	g_autoptr(GsChannel) c = NULL;

	c = gs_channel_new (name, version);
	gs_app_add_channel (app, c);
	if ((tracking_channel == NULL && gs_app_get_active_channel (app) == NULL) ||
	    g_strcmp0 (tracking_channel, name) == 0)
		gs_app_set_active_channel (app, c);
}

static gboolean
is_risk (const gchar *risk)
{
	return g_strcmp0 (risk, "stable") == 0 || g_strcmp0 (risk, "candidate") == 0 || g_strcmp0 (risk, "beta") == 0 || g_strcmp0 (risk, "edge") == 0;
}

static void
parse_channel_name (const gchar *name, gchar **track, gchar **risk, gchar **branch)
{
	g_auto(GStrv) tokens = NULL;

	tokens = g_strsplit (name, "/", -1);
	switch (g_strv_length (tokens)) {
	case 1:
		if (is_risk (tokens[0])) {
			if (track != NULL)
				*track = g_strdup ("latest");
			if (risk != NULL)
				*risk = g_strdup (tokens[0]);
		}
		else {
			if (track != NULL)
				*track = g_strdup (tokens[0]);
			if (risk != NULL)
				*risk = g_strdup ("stable");
		}
		if (branch != NULL)
			*branch = NULL;
		break;
	case 2:
		if (is_risk (tokens[0])) {
			if (track != NULL)
				*track = g_strdup ("latest");
			if (risk != NULL)
				*risk = g_strdup (tokens[0]);
			if (branch != NULL)
				*branch = g_strdup (tokens[1]);
		}
		else {
			if (track != NULL)
				*track = g_strdup (tokens[0]);
			if (risk != NULL)
				*risk = g_strdup (tokens[1]);
			if (branch != NULL)
				*branch = NULL;
		}
		break;
	case 3:
		if (track != NULL)
			*track = g_strdup (tokens[0]);
		if (risk != NULL)
			*risk = g_strdup (tokens[1]);
		if (branch != NULL)
			*branch = g_strdup (tokens[2]);
		break;
	default:
		break;
	}
}

static void
refine_channels (GsApp *app, JsonObject *snap, const gchar *tracking_channel)
{
	JsonArray *tracks;
	guint i;

	/* already refined... */
	if (gs_app_get_channels (app)->len > 0)
		return;

	tracks = json_object_get_array_member (snap, "tracks");
	for (i = 0; i < json_array_get_length (tracks); i++) {
		const gchar *track = json_array_get_string_element (tracks, i);
		const gchar *risks[] = {"stable", "candidate", "beta", "edge", NULL};
		g_autofree gchar *last_version = NULL;
		guint j;
		gboolean have_risk = FALSE;

		last_version = g_strdup (json_object_get_string_member (snap, "version"));
		for (j = 0; risks[j] != NULL; j++) {
			const gchar *risk = risks[j];
			JsonObject *channels;
			g_autofree gchar *name = NULL;
			g_autofree gchar *version = NULL;
			JsonObjectIter channel_iter;
			const gchar *channel_name;
			JsonNode *channel_node;

			channels = json_object_get_object_member (snap, "channels");

			if (strcmp (track, "latest") == 0)
				name = g_strdup (risk);
			else
				name = g_strdup_printf ("%s/%s", track, risk);
			json_object_iter_init (&channel_iter, channels);
			while (json_object_iter_next (&channel_iter, &channel_name, &channel_node)) {
				JsonObject *channel = json_node_get_object (channel_node);
				if (strcmp (json_object_get_string_member (channel, "channel"), name) == 0) {
					version = g_strdup (json_object_get_string_member (channel, "version"));
					break;
				}
			}
			if (version == NULL) {
				if (!have_risk)
					continue;
				version = g_strdup (last_version);
			}
			have_risk = TRUE;
			add_channel (app, name, version, tracking_channel);

			/* add any branches for this track/risk */
			json_object_iter_init (&channel_iter, channels);
			while (json_object_iter_next (&channel_iter, &channel_name, &channel_node)) {
				JsonObject *channel = json_node_get_object (channel_node);
				g_autofree gchar *channel_track = NULL;
				g_autofree gchar *channel_risk = NULL;
				g_autofree gchar *channel_branch = NULL;

				parse_channel_name (channel_name, &channel_track, &channel_risk, &channel_branch);
				if (channel_branch != NULL &&
				    g_strcmp0 (channel_track, track) == 0 &&
				    g_strcmp0 (channel_risk, risk) == 0) {
					add_channel (app, json_object_get_string_member (channel, "channel"), json_object_get_string_member (channel, "version"), tracking_channel);
				}
			}

			g_free (last_version);
			last_version = g_strdup (version);
		}
	}
}

gboolean
gs_plugin_refine_app (GsPlugin *plugin,
		      GsApp *app,
		      GsPluginRefineFlags flags,
		      GCancellable *cancellable,
		      GError **error)
{
	const gchar *id, *tracking_channel = NULL, *name;
	g_autofree gchar *store_version = NULL;
	g_autoptr(JsonObject) local_snap = NULL;
	g_autoptr(JsonObject) store_snap = NULL;
	JsonObject *snap;

	/* not us */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snap") != 0)
		return TRUE;

	id = gs_app_get_id (app);
	if (id == NULL)
		id = gs_app_get_source_default (app);
	if (id == NULL)
		return TRUE;

	/* get information from local snaps and store */
	local_snap = gs_snapd_list_one (id, cancellable, NULL);
	store_snap = get_store_snap (plugin, id, cancellable, NULL);

	/* get latest upstream version */
	if (local_snap != NULL)
		tracking_channel = json_object_get_string_member (local_snap, "tracking-channel");
	else
		tracking_channel = gs_app_get_metadata_item (app, "snap::channel");
	if (store_snap != NULL) {
		refine_channels (app, store_snap, tracking_channel);

		if (gs_app_get_active_channel (app) != NULL)
			store_version = g_strdup (gs_channel_get_version (gs_app_get_active_channel (app)));
		else
			store_version = g_strdup (json_object_get_string_member (store_snap, "version"));
	}

	gs_app_set_update_version (app, NULL);
	if (local_snap != NULL) {
		if (store_version != NULL && g_strcmp0 (store_version, json_object_get_string_member (local_snap, "version")) != 0) {
			gs_app_set_update_version (app, store_version);
			gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);
		}
		else {
			// Workaround it not being valid to switch from updatable to installed (e.g. if you switch channels)
			if (gs_app_get_state (app) == AS_APP_STATE_UPDATABLE_LIVE)
				gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
			gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		}
	}
	else
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);

	/* use store information for basic metadata over local information */
	snap = store_snap != NULL ? store_snap : local_snap;
	name = get_snap_title (snap);
	gs_app_set_name (app, GS_APP_QUALITY_NORMAL, name);
	gs_app_set_summary (app, GS_APP_QUALITY_NORMAL, json_object_get_string_member (snap, "summary"));
	gs_app_set_description (app, GS_APP_QUALITY_NORMAL, json_object_get_string_member (snap, "description"));
	if (json_object_has_member (snap, "license"))
		gs_app_set_license (app, GS_APP_QUALITY_NORMAL, json_object_get_string_member (snap, "license"));
	if (json_object_has_member (snap, "developer"))
		gs_app_set_developer_name (app, json_object_get_string_member (snap, "developer"));

	snap = local_snap != NULL ? local_snap : store_snap;
	gs_app_set_version (app, json_object_get_string_member (snap, "version"));

	/* add information specific to installed snaps */
	if (local_snap != NULL) {
		JsonArray *apps = NULL;
		g_autoptr(GDateTime) install_date = NULL;
		const gchar *launch_name = NULL;

		if (json_object_has_member (local_snap, "installed-size"))
			gs_app_set_size (app, json_object_get_int_member (local_snap, "installed-size"));
		if (json_object_has_member (local_snap, "install-date"))
			install_date = gs_snapd_parse_date (json_object_get_string_member (local_snap, "install-date"));
		if (install_date != NULL)
			gs_app_set_install_date (app, g_date_time_to_unix (install_date));

		if (json_object_has_member (local_snap, "apps"))
			apps = json_object_get_array_member (local_snap, "apps");
		if (apps && json_array_get_length (apps) > 0)
			launch_name = json_object_get_string_member (json_array_get_object_element (apps, 0), "name");

		if (launch_name)
			gs_app_set_metadata (app, "snap::launch-name", launch_name);
		else
			gs_app_add_quirk (app, AS_APP_QUIRK_NOT_LAUNCHABLE);
	}

	/* add information specific to store snaps */
	if (store_snap != NULL) {
		if (gs_app_get_size (app) == GS_APP_SIZE_UNKNOWN && json_object_has_member (store_snap, "download-size"))
			gs_app_set_size (app, json_object_get_int_member (store_snap, "download-size"));

		if (json_object_has_member (store_snap, "screenshots") && gs_app_get_screenshots (app)->len == 0) {
			JsonArray *screenshots;
			guint i;

			screenshots = json_object_get_array_member (store_snap, "screenshots");
			for (i = 0; i < json_array_get_length (screenshots); i++) {
				JsonObject *screenshot = json_array_get_object_element (screenshots, i);
				const gchar *url;
				g_autofree gchar *filename = NULL;
				g_autoptr(AsScreenshot) ss = NULL;
				g_autoptr(AsImage) image = NULL;

				/* skip sceenshots used for banner when app is featured */
				url = json_object_get_string_member (screenshot, "url");
				filename = g_path_get_basename (url);
				if (is_banner_image (filename) || is_banner_icon_image (filename))
					continue;

				ss = as_screenshot_new ();
				as_screenshot_set_kind (ss, AS_SCREENSHOT_KIND_NORMAL);
				image = as_image_new ();
				as_image_set_url (image, json_object_get_string_member (screenshot, "url"));
				as_image_set_kind (image, AS_IMAGE_KIND_SOURCE);
				if (json_object_has_member (screenshot, "width"))
					as_image_set_width (image, json_object_get_int_member (screenshot, "width"));
				if (json_object_has_member (screenshot, "height"))
					as_image_set_height (image, json_object_get_int_member (screenshot, "height"));
				as_screenshot_add_image (ss, image);
				gs_app_add_screenshot (app, ss);
			}
		}

		gs_app_set_origin (app, plugin->priv->store_name);
	}

	/* load icon if requested */
	if (gs_app_get_pixbuf (app) == NULL && gs_app_get_icon (app) == NULL) {
		const gchar *icon_url = NULL;

		if (local_snap != NULL && json_object_has_member (local_snap, "icon")) {
			icon_url = json_object_get_string_member (local_snap, "icon");
			if (g_strcmp0 (icon_url, "") == 0)
				icon_url = NULL;
		}
		if (icon_url == NULL && store_snap != NULL && json_object_has_member (store_snap, "icon")) {
			icon_url = json_object_get_string_member (store_snap, "icon");
			if (g_strcmp0 (icon_url, "") == 0)
				icon_url = NULL;
		}
		if (!load_icon (plugin, app, icon_url, cancellable, error))
			return FALSE;
	}

	return TRUE;
}

typedef struct
{
	GsPlugin *plugin;
	GsApp *app;
} ProgressData;

static void
progress_cb (JsonObject *result, gpointer user_data)
{
	ProgressData *data = user_data;
	JsonArray *tasks;
	GList *task_list, *l;
	gint64 done = 0, total = 0;

	tasks = json_object_get_array_member (result, "tasks");
	task_list = json_array_get_elements (tasks);

	for (l = task_list; l != NULL; l = l->next) {
		JsonObject *task, *progress;
		gint64 task_done, task_total;

		task = json_node_get_object (l->data);
		progress = json_object_get_object_member (task, "progress");
		task_done = json_object_get_int_member (progress, "done");
		task_total = json_object_get_int_member (progress, "total");

		done += task_done;
		total += task_total;
	}

	gs_plugin_progress_update (data->plugin, data->app, 100 * done / total);

	g_list_free (task_list);
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	ProgressData data;
	gboolean classic = FALSE;
	const gchar *channel = NULL;

	/* We can only install apps we know of */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snap") != 0)
		return TRUE;

	if (gs_app_get_active_channel (app) != NULL)
		channel = gs_channel_get_name (gs_app_get_active_channel (app));

	if (gs_app_get_state (app) == AS_APP_STATE_UPDATABLE_LIVE) {
		gs_app_set_state (app, AS_APP_STATE_INSTALLING);
		data.plugin = plugin;
		data.app = app;
		if (!gs_snapd_refresh (gs_app_get_id (app), channel, progress_cb, &data, cancellable, error)) {
			gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);
			return FALSE;
		}
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	}
	else {
		gs_app_set_state (app, AS_APP_STATE_INSTALLING);
		if (g_strcmp0 (gs_app_get_metadata_item (app, "snap::confinement"), "classic") == 0)
			classic = TRUE;
		data.plugin = plugin;
		data.app = app;
		if (!gs_snapd_install (gs_app_get_id (app), classic, channel, progress_cb, &data, cancellable, error)) {
			gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
			return FALSE;
		}
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	}

	return TRUE;
}

// Check if an app is graphical by checking if it uses a known GUI interface.
// This doesn't necessarily mean that every binary uses this interfaces, but is probably true.
// https://bugs.launchpad.net/bugs/1595023
static gboolean
is_graphical (GsApp *app, GCancellable *cancellable)
{
	g_autoptr(JsonObject) result = NULL;
	JsonArray *plugs;
	guint i;
	g_autoptr(GError) error = NULL;

	result = gs_snapd_get_interfaces (cancellable, &error);
	if (result == NULL) {
		g_warning ("Failed to check interfaces: %s", error->message);
		return FALSE;
	}

	plugs = json_object_get_array_member (result, "plugs");
	for (i = 0; i < json_array_get_length (plugs); i++) {
		JsonObject *plug = json_array_get_object_element (plugs, i);
		const gchar *interface;

		// Only looks at the plugs for this snap
		if (g_strcmp0 (json_object_get_string_member (plug, "snap"), gs_app_get_id (app)) != 0)
			continue;

		interface = json_object_get_string_member (plug, "interface");
		if (interface == NULL)
			continue;

		if (g_strcmp0 (interface, "unity7") == 0 || g_strcmp0 (interface, "x11") == 0 || g_strcmp0 (interface, "mir") == 0)
			return TRUE;
	}

	return FALSE;
}

gboolean
gs_plugin_launch (GsPlugin *plugin,
		  GsApp *app,
		  GCancellable *cancellable,
		  GError **error)
{
	const gchar *launch_name;
	g_autofree gchar *binary_name = NULL;
	GAppInfoCreateFlags flags = G_APP_INFO_CREATE_NONE;
	g_autoptr(GAppInfo) info = NULL;

	/* We can only launch apps we know of */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snap") != 0)
		return TRUE;

	launch_name = gs_app_get_metadata_item (app, "snap::launch-name");
	if (!launch_name)
		return TRUE;

	if (g_strcmp0 (launch_name, gs_app_get_id (app)) == 0)
		binary_name = g_strdup_printf ("/snap/bin/%s", launch_name);
	else
		binary_name = g_strdup_printf ("/snap/bin/%s.%s", gs_app_get_id (app), launch_name);

	if (!is_graphical (app, cancellable))
		flags |= G_APP_INFO_CREATE_NEEDS_TERMINAL;
	info = g_app_info_create_from_commandline (binary_name, NULL, flags, error);
	if (info == NULL)
		return FALSE;

	return g_app_info_launch (info, NULL, NULL, error);
}

gboolean
gs_plugin_app_switch_channel (GsPlugin *plugin,
			      GsApp *app,
			      GsChannel *channel,
			      GCancellable *cancellable,
			      GError **error)
{
	/* We can only modify apps we know of */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snap") != 0)
		return TRUE;

	if (!gs_snapd_switch (gs_app_get_id (app), gs_channel_get_name (channel), NULL, NULL, cancellable, error))
		return FALSE;

	return TRUE;
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	ProgressData data;

	/* We can only remove apps we know of */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snap") != 0)
		return TRUE;

	gs_app_set_state (app, AS_APP_STATE_REMOVING);
	data.plugin = plugin;
	data.app = app;
	if (!gs_snapd_remove (gs_app_get_id (app), progress_cb, &data, cancellable, error)) {
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		return FALSE;
	}
	gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	return TRUE;
}
