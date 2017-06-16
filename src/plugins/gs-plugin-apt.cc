/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Canonical Ltd
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

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>

#include <apt-pkg/init.h>
#include <apt-pkg/cachefile.h>
#include <apt-pkg/cmndline.h>
#include <apt-pkg/version.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <set>
#include <vector>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>

#include <gs-plugin.h>
#include <gs-utils.h>

#define LICENSE_URL "http://www.ubuntu.com/about/about-ubuntu/licensing"

#define INFO_DIR "/var/lib/dpkg/info"

typedef struct
{
	GMutex pending_mutex;
	GCond pending_cond;

	GMutex dispatched_mutex;
	GCond dispatched_cond;

	GMutex hashtable_mutex;

	guint still_to_read;
	guint dispatched_reads;

	GsPlugin *plugin;
} ReadListData;

typedef struct {
	gchar		*name;
	gchar		*section;
	gchar		*installed_version;
	gchar		*update_version;
	gchar		*origin;
	gchar		*release;
	gchar		*component;
	guint64		 installed_size;
} PackageInfo;

#include "ubuntu-unity-launcher-proxy.h"

struct GsPluginPrivate {
	GMutex		 mutex;
	gboolean	 loaded;
	GHashTable	*package_info;
	GHashTable	*installed_files;
	GList 		*installed_packages;
	GList 		*updatable_packages;
};

const gchar *
gs_plugin_get_name (void)
{
	return "apt";
}

const gchar **
gs_plugin_order_after (GsPlugin *plugin)
{
	static const gchar *deps[] = {
		"appstream",		/* need pkgname */
		NULL };
	return deps;
}

/**
 * gs_plugin_get_conflicts:
 */
const gchar **
gs_plugin_get_conflicts (GsPlugin *plugin)
{

	static const gchar *deps[] = {
		"packagekit",
		"packagekit-history",
		"packagekit-offline",
		"packagekit-origin",
		"packagekit-proxy",
		"packagekit-refine",
		"packagekit-refresh",
		"systemd-updates",
		NULL };
	return deps;
}

static void
free_package_info (gpointer data)
{
	PackageInfo *info = (PackageInfo *) data;
	g_free (info->section);
	g_free (info->installed_version);
	g_free (info->update_version);
	g_free (info->origin);
	g_free (info->release);
	g_free (info->component);
	g_free (info->name);
	g_free (info);
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
	plugin->priv = GS_PLUGIN_GET_PRIVATE (GsPluginPrivate);
	plugin->priv->package_info = g_hash_table_new_full (g_str_hash,
							    g_str_equal,
							    NULL,
							    free_package_info);

	plugin->priv->installed_files = g_hash_table_new_full (g_str_hash,
							    g_str_equal,
							    g_free,
							    g_free);

	g_mutex_init (&plugin->priv->mutex);

	pkgInitConfig (*_config);
	pkgInitSystem (*_config, _system);
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	g_mutex_lock (&plugin->priv->mutex);
	plugin->priv->loaded = FALSE;
	g_clear_pointer (&plugin->priv->package_info, g_hash_table_unref);
	g_clear_pointer (&plugin->priv->installed_files, g_hash_table_unref);
	g_clear_pointer (&plugin->priv->installed_packages, g_list_free);
	g_clear_pointer (&plugin->priv->updatable_packages, g_list_free);
	g_mutex_unlock (&plugin->priv->mutex);
	g_mutex_clear (&plugin->priv->mutex);
}


static void
read_list_file_cb (GObject *object,
		   GAsyncResult *res,
		   gpointer user_data)
{
	g_autoptr(GFileInputStream) stream = NULL;
	g_autoptr(GFile) file = NULL;
	ReadListData *data;
	g_autofree gchar *buffer = NULL;
	g_autofree gchar *filename = NULL;
	g_autoptr(GFileInfo) info = NULL;
	g_auto(GStrv) file_lines = NULL;
	g_auto(GStrv) file_components = NULL;

	file = G_FILE (object);
	data = (ReadListData *) user_data;
	stream = g_file_read_finish (file, res, NULL);

	info = g_file_input_stream_query_info (stream,
					       G_FILE_ATTRIBUTE_STANDARD_SIZE,
					       NULL,
					       NULL);

	if (!info)
		return;

	if (!g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
		return;

	buffer = (gchar *) g_malloc0 (g_file_info_get_size (info) + 1);

	if (!g_input_stream_read_all (G_INPUT_STREAM (stream),
				      buffer,
				      g_file_info_get_size (info),
				      NULL,
				      NULL,
				      NULL))
		return;

	g_input_stream_close (G_INPUT_STREAM (stream), NULL, NULL);

	file_lines = g_strsplit (buffer, "\n", -1);

	filename = g_file_get_basename (file);
	file_components = g_strsplit (filename, ".", 2);

	for (int i = 0; file_lines[i]; ++i)
		if (g_str_has_suffix (file_lines[i], ".desktop") ||
		    g_str_has_suffix (file_lines[i], ".metainfo.xml") ||
		    g_str_has_suffix (file_lines[i], ".appdata.xml"))
		{
			g_mutex_lock (&data->hashtable_mutex);
			/* filename -> package */
			g_hash_table_insert (data->plugin->priv->installed_files,
					     g_strdup (file_lines[i]),
					     g_strdup (file_components[0]));
			g_mutex_unlock (&data->hashtable_mutex);
		}

	g_mutex_lock (&data->dispatched_mutex);
	(data->dispatched_reads)--;
	g_cond_signal(&data->dispatched_cond);
	g_mutex_unlock(&data->dispatched_mutex);

	g_mutex_lock (&data->pending_mutex);
	(data->still_to_read)--;
	g_cond_signal (&data->pending_cond);
	g_mutex_unlock (&data->pending_mutex);
}

static void
read_list_file (GList *files,
		ReadListData *data)
{
	GFile *gfile;
	GList *files_iter;

	for (files_iter = files; files_iter; files_iter = files_iter->next)
	{
		/* freed in read_list_file_cb */
		gfile = g_file_new_for_path ((gchar *) files_iter->data);

		g_mutex_lock (&data->dispatched_mutex);
		g_file_read_async (gfile,
				G_PRIORITY_DEFAULT,
				NULL,
				read_list_file_cb,
				data);

		(data->dispatched_reads)++;

		while (data->dispatched_reads >= 500)
			g_cond_wait (&data->dispatched_cond, &data->dispatched_mutex);

		g_mutex_unlock (&data->dispatched_mutex);
	}
}

static void
look_for_files (GsPlugin *plugin)
{

	ReadListData data;
	GList *files = NULL;

	data.still_to_read = 0;
	data.dispatched_reads = 0;
	g_cond_init (&data.pending_cond);
	g_mutex_init (&data.pending_mutex);
	g_cond_init (&data.dispatched_cond);
	g_mutex_init (&data.dispatched_mutex);
	g_mutex_init (&data.hashtable_mutex);
	data.plugin = plugin;

	g_autoptr (GDir) dir = NULL;
	const gchar *file;

	dir = g_dir_open (INFO_DIR, 0, NULL);

	while (file = g_dir_read_name (dir))
		if (g_str_has_suffix (file, ".list") &&
		   /* app-install-data contains loads of .desktop files, but they aren't installed by it */
		   (!g_strcmp0 (file, "app-install-data.list") == 0))
		{
			files = g_list_append (files, g_build_filename (INFO_DIR, file, NULL));
			data.still_to_read++;
		}

	read_list_file (files, &data);

	/* Wait until all the reads are done */
	g_mutex_lock (&data.pending_mutex);
	while (data.still_to_read > 0)
		g_cond_wait (&data.pending_cond, &data.pending_mutex);
	g_mutex_unlock (&data.pending_mutex);

	g_mutex_clear (&data.pending_mutex);
	g_cond_clear (&data.pending_cond);
	g_mutex_clear (&data.dispatched_mutex);
	g_cond_clear (&data.dispatched_cond);
	g_mutex_clear (&data.hashtable_mutex);

	g_list_free_full (files, g_free);
}

static gboolean
version_newer (const gchar *v0, const gchar *v1)
{
	return v0 ? _system->VS->CmpVersion(v0, v1) < 0 : TRUE;
}

/* return FALSE for a fatal error */
static gboolean
look_at_pkg (const pkgCache::PkgIterator &P,
	     pkgSourceList *list,
	     pkgPolicy *policy,
	     GsPlugin *plugin,
	     GError **error)
{
	pkgCache::VerIterator current = P.CurrentVer();
	pkgCache::VerIterator candidate = policy->GetCandidateVer(P);
	pkgCache::VerFileIterator VF;
	FileFd PkgF;
	pkgTagSection Tags;
	gchar *name;

	PackageInfo *info;

	if (!candidate || !candidate.IsGood () || !candidate.FileList ())
		return TRUE;

	name = g_strdup (P.Name ());
	info = (PackageInfo *) g_hash_table_lookup (plugin->priv->package_info, name);
	if (info == NULL) {
		info = g_new0 (PackageInfo, 1);
		info->name = name;
		g_hash_table_insert (plugin->priv->package_info, name, info);
	} else
		g_free (name);

	for (VF = candidate.FileList (); VF.IsGood (); VF++) {
		// see InRelease for the fields
		if (VF.File ().Archive ())
			info->release = g_strdup (VF.File ().Archive ());
		if (VF.File ().Origin ())
			info->origin = g_strdup (VF.File ().Origin ());
		if (VF.File ().Component ())
			info->component = g_strdup (VF.File ().Component ());
		// also available: Codename, Label
		break;
	}


	pkgCache::PkgFileIterator I = VF.File ();

	if (I.IsOk () == false) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     ("apt DB load failed: package file %s is out of sync."), I.FileName ());
		return FALSE;
	}

	PkgF.Open (I.FileName (), FileFd::ReadOnly, FileFd::Extension);

	pkgTagFile TagF (&PkgF);

	if (!current.IsGood() || TagF.Jump (Tags, current.FileList ()->Offset) == false) {
		if (TagF.Jump (Tags, candidate.FileList ()->Offset) == false)
			return TRUE;
	}

	if (Tags.Exists ("Installed-Size"))
		info->installed_size = Tags.FindULL ("Installed-Size") * 1024;
	else
		info->installed_size = 0;

	if (current && current.IsGood ())
		info->installed_version = g_strdup (current.VerStr ());
	if (candidate)
		info->update_version = g_strdup (candidate.VerStr ());

	info->section = g_strdup (candidate.Section ());
	if (info->installed_version) {
		plugin->priv->installed_packages =
			g_list_append (plugin->priv->installed_packages, info);
	}

	/* no upgrade */
	if (g_strcmp0 (info->installed_version, info->update_version) == 0)
		g_clear_pointer (&info->update_version, g_free);

	if (info->installed_version && info->update_version && P->SelectedState != pkgCache::State::Hold)
		plugin->priv->updatable_packages =
			g_list_append (plugin->priv->updatable_packages, info);

	return TRUE;
}

static gboolean
load_apt_db (GsPlugin *plugin, GError **error)
{
	pkgSourceList *list;
	pkgPolicy *policy;
	pkgCacheFile cachefile;
	pkgCache *cache;
	pkgCache::PkgIterator P;
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&plugin->priv->mutex);

	if (plugin->priv->loaded)
		return TRUE;

	_error->Discard();
	cache = cachefile.GetPkgCache();
	list = cachefile.GetSourceList();
	policy = cachefile.GetPolicy();
	if (cache == NULL || _error->PendingError()) {
		_error->DumpErrors();
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "apt DB load failed: error while initialising");
		return FALSE;
	}

	for (pkgCache::GrpIterator grp = cache->GrpBegin(); grp != cache->GrpEnd(); grp++) {
		P = grp.FindPreferredPkg();
		if (P.end())
			continue;
		if (!look_at_pkg (P, list, policy, plugin, error))
			return FALSE;
	}

	/* load filename -> package map into plugin->priv->installed_files */
	look_for_files (plugin);

	plugin->priv->loaded = TRUE;
	return TRUE;
}

static void
unload_apt_db (GsPlugin *plugin)
{
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&plugin->priv->mutex);

	plugin->priv->loaded = FALSE;
	g_hash_table_remove_all (plugin->priv->package_info);
	g_hash_table_remove_all (plugin->priv->installed_files);
	g_clear_pointer (&plugin->priv->installed_packages, g_list_free);
	g_clear_pointer (&plugin->priv->updatable_packages, g_list_free);
}

static void
get_changelog (GsPlugin *plugin, GsApp *app)
{
	guint i;
	guint status_code;
	g_autofree gchar *binary_source = NULL;
	g_autofree gchar *changelog_prefix = NULL;
	g_autofree gchar *current_version = NULL;
	g_autofree gchar *source_prefix = NULL;
	g_autofree gchar *update_version = NULL;
	g_autofree gchar *uri = NULL;
	g_auto(GStrv) lines = NULL;
	g_autoptr(GString) details = NULL;
	g_autoptr(SoupMessage) msg = NULL;

	// Need to know the source and version to download changelog
	binary_source = g_strdup (gs_app_get_source_default (app));
	current_version = g_strdup (gs_app_get_version (app));
	update_version = g_strdup (gs_app_get_update_version (app));
	if (binary_source == NULL || update_version == NULL)
		return;

	if (g_str_has_prefix (binary_source, "lib"))
		source_prefix = g_strdup_printf ("lib%c", binary_source[3]);
	else
		source_prefix = g_strdup_printf ("%c", binary_source[0]);
	uri = g_strdup_printf ("http://changelogs.ubuntu.com/changelogs/binary/%s/%s/%s/changelog", source_prefix, binary_source, update_version);

	/* download file */
	msg = soup_message_new (SOUP_METHOD_GET, uri);
	status_code = soup_session_send_message (plugin->soup_session, msg);
	if (status_code != SOUP_STATUS_OK) {
		g_warning ("Failed to get changelog for %s version %s from changelogs.ubuntu.com: %s", binary_source, update_version, soup_status_get_phrase (status_code));
		return;
	}

	// Extract changelog entries newer than our current version
	lines = g_strsplit (msg->response_body->data, "\n", -1);
	details = g_string_new ("");
	for (i = 0; lines[i] != NULL; i++) {
		gchar *line = lines[i];
		const gchar *version_start, *version_end;
		g_autofree gchar *v = NULL;

		// First line is in the form "package (version) distribution(s); urgency=urgency"
                version_start = strchr (line, '(');
                version_end = strchr (line, ')');
		if (line[0] == ' ' || version_start == NULL || version_end == NULL || version_end < version_start)
			continue;
		v = g_strdup_printf ("%.*s", (int) (version_end - version_start - 1), version_start + 1);

		// We're only interested in new versions
		if (!version_newer (current_version, v))
			break;

		g_string_append_printf (details, "%s\n", v);
		for (i++; lines[i] != NULL; i++) {
			// Last line is in the form " -- maintainer name <email address>  date"
			if (g_str_has_prefix (lines[i], " -- "))
				break;
			g_string_append_printf (details, "%s\n", lines[i]);
		}
	}

	gs_app_set_update_details (app, details->str);
}

static gboolean
is_open_source (PackageInfo *info)
{
	const gchar *open_source_components[] = { "main", "universe", NULL };

	/* There's no valid apps in the libs section */
	return info->component != NULL && g_strv_contains (open_source_components, info->component);
}

static gchar *
get_origin (PackageInfo *info)
{
	if (!info->origin)
		return NULL;

	g_autofree gchar *origin_lower = g_strdup (info->origin);
	for (int i = 0; origin_lower[i]; ++i)
		origin_lower[i] = g_ascii_tolower (origin_lower[i]);

	return g_strdup_printf ("%s-%s-%s", origin_lower, info->release, info->component);
}

gboolean
gs_plugin_refine_app (GsPlugin *plugin,
		      GsApp *app,
		      GsPluginRefineFlags flags,
		      GCancellable *cancellable,
		      GError **error)
{
	GList *link;
	PackageInfo *info;
	const gchar *tmp;
	g_autoptr(GMutexLocker) locker = NULL;
	g_autofree gchar *fn = NULL;
	g_autofree gchar *origin = NULL;
	gchar *package = NULL;

	if (!load_apt_db (plugin, error))
		return FALSE;

	locker = g_mutex_locker_new (&plugin->priv->mutex);

	tmp = gs_app_get_id (app);
	if (gs_app_get_source_id_default (app) == NULL && tmp) {
		switch (gs_app_get_kind (app)) {
		case AS_APP_KIND_DESKTOP:
			fn = g_strdup_printf ("/usr/share/applications/%s", tmp);
			break;
		case AS_APP_KIND_ADDON:
			fn = g_strdup_printf ("/usr/share/appdata/%s.metainfo.xml", tmp);
			break;
		default:
			break;
		}

		if (!fn || !g_file_test (fn, G_FILE_TEST_EXISTS)) {
			g_debug ("ignoring %s as does not exist", fn);
		} else {
			package = (gchar *) g_hash_table_lookup (plugin->priv->installed_files,
								 fn);
			if (package != NULL) {
				gs_app_add_source (app, package);
				gs_app_set_management_plugin (app, "apt");
			}
		}
	}

	if (gs_app_get_source_default (app) == NULL)
		return TRUE;

	info = (PackageInfo *) g_hash_table_lookup (plugin->priv->package_info, gs_app_get_source_default (app));
	if (info == NULL)
		return TRUE;

	origin = get_origin (info);
	gs_app_set_origin (app, origin);
	gs_app_set_origin_ui (app, info->origin);

	if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN) {
		if (info->installed_version != NULL) {
			if (info->update_version != NULL) {
				gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);
			} else {
				gs_app_set_state (app, AS_APP_STATE_INSTALLED);
			}
		} else {
			gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
		}
	}
	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN) != 0) {
		g_autofree gchar *origin = get_origin (info);
		gs_app_set_origin (app, origin);
		gs_app_set_origin_ui (app, info->origin);
	}
	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE) != 0 && gs_app_get_size (app) == 0) {
		gs_app_set_size (app, info->installed_size);
	}
	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION) != 0) {
		if (info->installed_version != NULL) {
			gs_app_set_version (app, info->installed_version);
		} else {
			gs_app_set_version (app, info->update_version);
		}
		if (info->update_version != NULL) {
			gs_app_set_update_version (app, info->update_version);
		}
	}
	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_LICENSE) != 0 && is_open_source(info)) {
		gs_app_set_license (app, GS_APP_QUALITY_LOWEST, "@LicenseRef-free=" LICENSE_URL);
	}

	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_CHANGELOG) != 0) {
		get_changelog (plugin, app);
	}

	return TRUE;
}

static gboolean
is_allowed_section (PackageInfo *info)
{
	const gchar *section_blacklist[] = { "libs", NULL };

	/* There's no valid apps in the libs section */
	return info->section == NULL || !g_strv_contains (section_blacklist, info->section);
}

gboolean
gs_plugin_add_installed (GsPlugin *plugin,
			 GList **list,
			 GCancellable *cancellable,
			 GError **error)
{
	GList *link;
	g_autoptr(GMutexLocker) locker = NULL;

	if (!load_apt_db (plugin, error))
		return FALSE;

	locker = g_mutex_locker_new (&plugin->priv->mutex);

	for (link = plugin->priv->installed_packages; link; link = link->next) {
		PackageInfo *info = (PackageInfo *) link->data;
		g_autofree gchar *origin = get_origin (info);
		g_autoptr(GsApp) app = NULL;

		if (!is_allowed_section (info))
			continue;

		app = gs_app_new (NULL);
		gs_app_set_management_plugin (app, "apt");
		gs_app_set_name (app, GS_APP_QUALITY_LOWEST, info->name);
		gs_app_add_source (app, info->name);
		gs_app_set_origin (app, origin);
		gs_app_set_origin_ui (app, info->origin);
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		gs_plugin_add_app (list, app);
	}

	return TRUE;
}

typedef struct {
	GCancellable *cancellable;
	GSocket *debconf_connection;
	GSource *debconf_read_source;
	GSource *debconf_client_read_source;
	GPid debconf_pid;
	gint stdin_pipe;
	gint stdout_pipe;
	gint stderr_pipe;
} DebconfData;

static DebconfData *
debconf_data_new (GCancellable *cancellable)
{
	DebconfData *data;

	data = g_slice_new0 (DebconfData);
	data->cancellable = (GCancellable *) g_object_ref (cancellable);
	data->stdin_pipe = -1;
	data->stdout_pipe = -1;
	data->stderr_pipe = -1;

	return data;
}

static void
debconf_data_free (DebconfData *data)
{
	if (data->debconf_read_source != NULL)
		g_source_destroy (data->debconf_read_source);
	if (data->debconf_client_read_source != NULL)
		g_source_destroy (data->debconf_client_read_source);

	g_clear_object (&data->cancellable);
	if (data->debconf_connection != NULL)
		g_socket_close (data->debconf_connection, NULL);
	g_clear_object (&data->debconf_connection);
	g_clear_pointer (&data->debconf_read_source, g_source_unref);
	g_clear_pointer (&data->debconf_client_read_source, g_source_unref);
	if (data->stdin_pipe > 0)
		close (data->stdin_pipe);
	if (data->stdout_pipe > 0)
		close (data->stdout_pipe);
	if (data->stderr_pipe > 0)
		close (data->stderr_pipe);
	if (data->debconf_pid > 0)
		kill (data->debconf_pid, SIGTERM);
	g_slice_free (DebconfData, data);
}

typedef struct {
	GsPlugin *plugin;
	GCancellable *cancellable;
	GsApp *app;
	GsAppList *apps;
	gchar *result;
	GMainContext *context;
	GMainLoop *loop;
	GSocket *debconf_socket;
	GList *debconf_connections;
} TransactionData;

static TransactionData *
transaction_data_new (GsPlugin *plugin, GCancellable *cancellable)
{
	TransactionData *data;

	data = g_slice_new0 (TransactionData);
	data->plugin = plugin;
	data->cancellable = (GCancellable *) g_object_ref (cancellable);
	data->context = g_main_context_new ();
	data->loop = g_main_loop_new (data->context, FALSE);

	return data;
}

static void
transaction_data_free (TransactionData *data)
{
	g_clear_object (&data->cancellable);
	g_free (data->result);
	g_clear_pointer (&data->context, g_main_context_unref);
	g_clear_pointer (&data->loop, g_main_loop_unref);
	if (data->debconf_socket != NULL)
		g_socket_close (data->debconf_socket, NULL);
	g_clear_object (&data->debconf_socket);
	g_list_free_full (data->debconf_connections, (GDestroyNotify) debconf_data_free);
	g_slice_free (TransactionData, data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(TransactionData, transaction_data_free)

static void
transaction_property_changed_cb (GDBusConnection *connection,
				 const gchar *sender_name,
				 const gchar *object_path,
				 const gchar *interface_name,
				 const gchar *signal_name,
				 GVariant *parameters,
				 gpointer user_data)
{
	TransactionData *data = (TransactionData *) user_data;
	const gchar *name;
	GList *i;
	g_autoptr(GVariant) value = NULL;

	if (g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(sv)"))) {
		g_variant_get (parameters, "(&sv)", &name, &value);
		if (g_strcmp0 (name, "Progress") == 0) {
			if (data->app)
				gs_plugin_progress_update (data->plugin, data->app, g_variant_get_int32 (value));
			for (i = data->apps; i != NULL; i = i->next)
				gs_plugin_progress_update (data->plugin, GS_APP (i->data), g_variant_get_int32 (value));
		}
	} else {
		g_warning ("Unknown parameters in %s.%s: %s", interface_name, signal_name, g_variant_get_type_string (parameters));
	}
}

static void
transaction_finished_cb (GDBusConnection *connection,
			 const gchar *sender_name,
			 const gchar *object_path,
			 const gchar *interface_name,
			 const gchar *signal_name,
			 GVariant *parameters,
			 gpointer user_data)
{
	TransactionData *data = (TransactionData *) user_data;

	if (g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(s)")))
		g_variant_get (parameters, "(s)", &data->result);
	else
		g_warning ("Unknown parameters in %s.%s: %s", interface_name, signal_name, g_variant_get_type_string (parameters));

	g_main_loop_quit (data->loop);
}

static void
notify_unity_launcher (GsApp *app, const gchar *transaction_path)
{
	UbuntuUnityLauncher *launcher = NULL;

	g_return_if_fail (GS_IS_APP (app));
	g_return_if_fail (transaction_path);

	launcher = ubuntu_unity_launcher_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
		G_DBUS_PROXY_FLAGS_NONE,
		"com.canonical.Unity.Launcher",
		"/com/canonical/Unity/Launcher",
		NULL, NULL);

	g_return_if_fail (launcher);

	ubuntu_unity_launcher_call_add_launcher_item (launcher,
		gs_app_get_id (app),
		transaction_path,
		NULL, NULL, NULL);

	g_object_unref (launcher);
}

static gboolean
debconf_read_cb (GSocket *socket, GIOCondition condition, gpointer user_data)
{
	DebconfData *data = (DebconfData *) user_data;
	gchar buffer[1024];
	gssize n_read;
	g_autoptr(GError) error = NULL;

	n_read = g_socket_receive (socket, buffer, 1024, data->cancellable, &error);
	if (n_read == 0) {
		close (data->stdin_pipe);
		data->stdin_pipe = -1;
		return G_SOURCE_REMOVE;
	}
	if (n_read < 0) {
		g_warning ("Error reading from debconf socket: %s\n", g_strerror (errno));
		return G_SOURCE_CONTINUE;
	}

	if (write (data->stdin_pipe, buffer, n_read) < 0) {
		g_warning ("Error writing to debconf client: %s\n", error->message);
		return G_SOURCE_CONTINUE;
	}

	return G_SOURCE_CONTINUE;
}

static gboolean
debconf_client_read_cb (gint fd, GIOCondition condition, gpointer user_data)
{
	DebconfData *data = (DebconfData *) user_data;
	gchar buffer[1024];
	gssize n_read;
	g_autoptr(GError) error = NULL;

	n_read = read (fd, buffer, 1024);
	if (n_read < 0) {
		g_warning ("Error reading from debconf client: %s\n", g_strerror (errno));
		return G_SOURCE_CONTINUE;
	}
	if (n_read == 0) {
		g_socket_close (data->debconf_connection, NULL);
		return G_SOURCE_REMOVE;
	}

	if (!g_socket_send (data->debconf_connection, buffer, n_read, data->cancellable, &error)) {
		g_warning ("Error writing to debconf socket: %s\n", error->message);
		return G_SOURCE_CONTINUE;
	}

	return G_SOURCE_CONTINUE;
}

static gboolean
debconf_accept_cb (GSocket *socket, GIOCondition condition, gpointer user_data)
{
	TransactionData *data = (TransactionData *) user_data;
	DebconfData *debconf_data;
	g_autoptr(GPtrArray) argv = NULL;
	g_auto(GStrv) envp = NULL;
	g_autoptr(GError) error = NULL;

	debconf_data = debconf_data_new (data->cancellable);
	data->debconf_connections = g_list_append (data->debconf_connections, debconf_data);
	debconf_data->debconf_connection = g_socket_accept (socket, data->cancellable, &error);
	if (debconf_data->debconf_connection == NULL) {
		g_warning ("Failed to accept debconf connection: %s", error->message);
		return G_SOURCE_CONTINUE;
	}
	debconf_data->debconf_read_source = g_socket_create_source (debconf_data->debconf_connection, G_IO_IN, data->cancellable);
	g_source_set_callback (debconf_data->debconf_read_source, (GSourceFunc) debconf_read_cb, debconf_data, NULL);
	g_source_attach (debconf_data->debconf_read_source, data->context);

	argv = g_ptr_array_new ();
	g_ptr_array_add (argv, (gpointer) "debconf-communicate");
	g_ptr_array_add (argv, NULL);
	envp = g_get_environ ();
	envp = g_environ_setenv (envp, "DEBCONF_DB_REPLACE", "configdb", TRUE);
	envp = g_environ_setenv (envp, "DEBCONF_DB_OVERRIDE", "Pipe{infd:none outfd:none}", TRUE);
	envp = g_environ_setenv (envp, "DEBIAN_FRONTEND", "gnome", TRUE);
	if (!g_spawn_async_with_pipes (NULL,
	                               (gchar **) argv->pdata,
	                               envp,
	                               G_SPAWN_SEARCH_PATH,
	                               NULL, NULL,
	                               &debconf_data->debconf_pid,
	                               &debconf_data->stdin_pipe,
	                               &debconf_data->stdout_pipe,
	                               &debconf_data->stderr_pipe,
	                               &error)) {
		g_warning ("Failed to launch debconf-communicate: %s", error->message);
		g_socket_close (debconf_data->debconf_connection, NULL);
		return G_SOURCE_CONTINUE;
	}
	debconf_data->debconf_client_read_source = g_unix_fd_source_new (debconf_data->stdout_pipe, G_IO_IN);
	g_source_set_callback (debconf_data->debconf_client_read_source, (GSourceFunc) debconf_client_read_cb, debconf_data, NULL);
	g_source_attach (debconf_data->debconf_client_read_source, data->context);

	return G_SOURCE_CONTINUE;
}

static gboolean
aptd_transaction (GsPlugin     *plugin,
		  const gchar  *method,
		  GsApp        *app,
		  GList        *apps,
		  GVariant     *parameters,
		  GCancellable *cancellable,
		  GError      **error)
{
	g_autoptr(GDBusConnection) conn = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autofree gchar *transaction_path = NULL, *temp_dir = NULL, *debconf_socket_path = NULL;
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GSource) accept_source = NULL;
	guint property_signal, finished_signal;
	g_autoptr(TransactionData) data = NULL;

	conn = g_bus_get_sync (G_BUS_TYPE_SYSTEM, cancellable, error);
	if (conn == NULL)
		return FALSE;

	if (parameters == NULL && app != NULL)
		parameters = g_variant_new_parsed ("([%s],)", gs_app_get_source_default (app));

	result = g_dbus_connection_call_sync (conn,
					      "org.debian.apt",
					      "/org/debian/apt",
					      "org.debian.apt",
					      method,
					      parameters,
					      G_VARIANT_TYPE ("(s)"),
					      G_DBUS_CALL_FLAGS_NONE,
					      -1,
					      cancellable,
					      error);
	if (result == NULL)
		return FALSE;
	g_variant_get (result, "(s)", &transaction_path);
	g_clear_pointer (&result, g_variant_unref);

	data = transaction_data_new (plugin, cancellable);
	data->app = app;
	data->apps = apps;

	temp_dir = g_dir_make_tmp ("gnome-software-XXXXXX", NULL);
	debconf_socket_path = g_build_filename (temp_dir, "debconf.socket", NULL);
	data->debconf_socket = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, error);
	address = g_unix_socket_address_new (debconf_socket_path);
	if (data->debconf_socket == NULL || !g_socket_bind (data->debconf_socket, address, FALSE, error))
		return FALSE;
	accept_source = g_socket_create_source (data->debconf_socket, G_IO_IN, cancellable);
	g_source_set_callback (accept_source, (GSourceFunc) debconf_accept_cb, data, NULL);
	g_source_attach (accept_source, data->context);
	if (!g_socket_listen (data->debconf_socket, error))
		return FALSE;
	result = g_dbus_connection_call_sync (conn,
                                              "org.debian.apt",
                                              transaction_path,
                                              "org.freedesktop.DBus.Properties",
                                              "Set",
                                              g_variant_new ("(ssv)", "org.debian.apt.transaction", "DebconfSocket", g_variant_new_string (debconf_socket_path)),
                                              G_VARIANT_TYPE ("()"),
                                              G_DBUS_CALL_FLAGS_NONE,
					      -1,
					      cancellable,
					      error);
	if (result == NULL)
		return FALSE;
	g_clear_pointer (&result, g_variant_unref);

	if (!g_strcmp0(method, "InstallPackages"))
		notify_unity_launcher (app, transaction_path);

	property_signal = g_dbus_connection_signal_subscribe (conn,
							      "org.debian.apt",
							      "org.debian.apt.transaction",
							      "PropertyChanged",
							      transaction_path,
							      NULL,
							      G_DBUS_SIGNAL_FLAGS_NONE,
							      transaction_property_changed_cb,
							      data,
							      NULL);

	finished_signal = g_dbus_connection_signal_subscribe (conn,
							      "org.debian.apt",
							      "org.debian.apt.transaction",
							      "Finished",
							      transaction_path,
							      NULL,
							      G_DBUS_SIGNAL_FLAGS_NONE,
							      transaction_finished_cb,
							      data,
							      NULL);

	result = g_dbus_connection_call_sync (conn,
					      "org.debian.apt",
					      transaction_path,
					      "org.debian.apt.transaction",
					      "Run",
					      g_variant_new ("()"),
					      G_VARIANT_TYPE ("()"),
					      G_DBUS_CALL_FLAGS_NONE,
					      -1,
					      cancellable,
					      error);
	if (result != NULL)
		g_main_loop_run (data->loop);
	g_dbus_connection_signal_unsubscribe (conn, property_signal);
	g_dbus_connection_signal_unsubscribe (conn, finished_signal);
	if (result == NULL)
		return FALSE;

	if (g_strcmp0 (data->result, "exit-success") != 0) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "apt transaction returned result %s", data->result);
		return FALSE;
	}

	return TRUE;
}

static gboolean
app_is_ours (GsApp *app)
{
	const gchar *management_plugin = gs_app_get_management_plugin (app);

	// FIXME: Since appstream marks all packages as owned by PackageKit and
	// we are replacing PackageKit we need to accept those packages
	const gchar *our_management_plugins[] = { "packagekit", "apt", NULL };

	return g_strv_contains (our_management_plugins, management_plugin);
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	g_autofree gchar *filename = NULL;
	gboolean success = FALSE;

	if (!app_is_ours (app))
		return TRUE;

	if (gs_app_get_source_default (app) == NULL)
		return TRUE;

	switch (gs_app_get_state (app)) {
	case AS_APP_STATE_AVAILABLE:
	case AS_APP_STATE_UPDATABLE:
		gs_app_set_state (app, AS_APP_STATE_INSTALLING);
		success = aptd_transaction (plugin, "InstallPackages", app, NULL, NULL, cancellable, error);
		break;
	case AS_APP_STATE_AVAILABLE_LOCAL:
		filename = g_file_get_path (gs_app_get_local_file (app));
		gs_app_set_state (app, AS_APP_STATE_INSTALLING);
		success = aptd_transaction (plugin, "InstallFile", app, NULL,
					    g_variant_new_parsed ("(%s, true)", filename),
					    cancellable, error);
		break;
	default:
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "do not know how to install app in state %s",
			     as_app_state_to_string (gs_app_get_state (app)));
		return FALSE;
	}


	if (success)
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	else
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);

	return success;
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	if (!app_is_ours (app))
		return TRUE;

	if (gs_app_get_source_default (app) == NULL)
		return TRUE;

	gs_app_set_state (app, AS_APP_STATE_REMOVING);
	if (aptd_transaction (plugin, "RemovePackages", app, NULL, NULL, cancellable, error))
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	else {
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		return FALSE;
	}

	return TRUE;
}

gboolean
gs_plugin_refresh (GsPlugin *plugin,
		   guint cache_age,
		   GsPluginRefreshFlags flags,
		   GCancellable *cancellable,
		   GError **error)
{
	if ((flags & GS_PLUGIN_REFRESH_FLAGS_METADATA) == 0)
		return TRUE;

	if (!aptd_transaction (plugin, "UpdateCache", NULL, NULL, NULL, cancellable, error))
		return FALSE;

	unload_apt_db (plugin);

	gs_plugin_updates_changed (plugin);

	return TRUE;
}

gboolean
gs_plugin_add_updates (GsPlugin *plugin,
                       GList **list,
                       GCancellable *cancellable,
                       GError **error)
{
	GList *link;
	g_autoptr(GMutexLocker) locker = NULL;

	if (!load_apt_db (plugin, error))
		return FALSE;

	locker = g_mutex_locker_new (&plugin->priv->mutex);

	for (link = plugin->priv->updatable_packages; link; link = link->next) {
		PackageInfo *info = (PackageInfo *) link->data;
		g_autoptr(GsApp) app = NULL;

		if (!is_allowed_section (info))
			continue;

		app = gs_app_new (NULL);
		gs_app_set_management_plugin (app, "apt");
		gs_app_set_name (app, GS_APP_QUALITY_LOWEST, info->name);
		gs_app_set_kind (app, AS_APP_KIND_GENERIC);
		gs_app_add_source (app, info->name);
		gs_plugin_add_app (list, app);
	}

	return TRUE;
}

static void
set_list_state (GList      *apps,
		AsAppState  state)
{
	GList *i;
	guint j;
	GsApp *app_i;
	GsApp *app_j;
	GPtrArray *related;

	for (i = apps; i != NULL; i = i->next) {
		app_i = GS_APP (i->data);
		gs_app_set_state (app_i, state);

		if (g_strcmp0 (gs_app_get_id (app_i), "os-update.virtual") == 0) {
			related = gs_app_get_related (app_i);

			for (j = 0; j < related->len; j++) {
				app_j = GS_APP (g_ptr_array_index (related, j));
				gs_app_set_state (app_j, state);
			}
		}
	}
}

gboolean
gs_plugin_update (GsPlugin      *plugin,
		  GList         *apps,
		  GCancellable  *cancellable,
		  GError       **error)
{
	GList *i;
	GsApp *app_i;

	for (i = apps; i != NULL; i = i->next) {
		app_i = GS_APP (i->data);

		if (g_strcmp0 (gs_app_get_id (app_i), "os-update.virtual") == 0) {
			set_list_state (apps, AS_APP_STATE_INSTALLING);

			if (aptd_transaction (plugin, "UpgradeSystem", NULL, apps, g_variant_new_parsed ("(false,)"), cancellable, error)) {
				set_list_state (apps, AS_APP_STATE_INSTALLED);

				unload_apt_db (plugin);

				gs_plugin_updates_changed (plugin);

				return TRUE;
			} else {
				set_list_state (apps, AS_APP_STATE_UPDATABLE_LIVE);

				return FALSE;
			}
		}
	}

	return TRUE;
}

gboolean
gs_plugin_update_app (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	GPtrArray *apps;
	GsApp *app_i;
	guint i;
	GVariantBuilder builder;

	if (g_strcmp0 (gs_app_get_id (app), "os-update.virtual") == 0) {
		apps = gs_app_get_related (app);

		g_variant_builder_init (&builder, G_VARIANT_TYPE ("(as)"));
		g_variant_builder_open (&builder, G_VARIANT_TYPE ("as"));

		gs_app_set_state (app, AS_APP_STATE_INSTALLING);

		for (i = 0; i < apps->len; i++) {
			app_i = GS_APP (g_ptr_array_index (apps, i));
			gs_app_set_state (app_i, AS_APP_STATE_INSTALLING);
			g_variant_builder_add (&builder, "s", gs_app_get_source_default (app_i));
		}

		g_variant_builder_close (&builder);

		if (aptd_transaction (plugin, "UpgradePackages", app, NULL, g_variant_builder_end (&builder), cancellable, error)) {
			gs_app_set_state (app, AS_APP_STATE_INSTALLED);

			for (i = 0; i < apps->len; i++)
				gs_app_set_state (GS_APP (g_ptr_array_index (apps, i)), AS_APP_STATE_INSTALLED);

			unload_apt_db (plugin);

			gs_plugin_updates_changed (plugin);
		} else {
			gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);

			for (i = 0; i < apps->len; i++)
				gs_app_set_state (GS_APP (g_ptr_array_index (apps, i)), AS_APP_STATE_UPDATABLE_LIVE);

			return FALSE;
		}
	} else if (app_is_ours (app)) {
		gs_app_set_state (app, AS_APP_STATE_INSTALLING);

		if (aptd_transaction (plugin, "UpgradePackages", app, NULL, NULL, cancellable, error)) {
			gs_app_set_state (app, AS_APP_STATE_INSTALLED);

			unload_apt_db (plugin);

			gs_plugin_updates_changed (plugin);
		} else {
			gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);

			return FALSE;
		}
	}

	return TRUE;
}

gboolean
gs_plugin_launch (GsPlugin *plugin,
		  GsApp *app,
		  GCancellable *cancellable,
		  GError **error)
{
	if (!app_is_ours (app))
		return TRUE;

	return gs_plugin_app_launch (plugin, app, error);
}

/* vim: set noexpandtab ts=8 sw=8: */
