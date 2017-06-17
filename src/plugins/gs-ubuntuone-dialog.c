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

#include "gs-ubuntuone-dialog.h"
#include "gs-utils.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#ifdef USE_SNAPD
#include <snapd-glib/snapd-glib.h>
#include "gs-snapd.h"
#endif

#define UBUNTU_LOGIN_HOST "https://login.ubuntu.com"

struct _GsUbuntuoneDialog
{
	GtkDialog parent_instance;

	GtkWidget *content_box;
	GtkWidget *cancel_button;
	GtkWidget *next_button;
	GtkWidget *status_stack;
	GtkWidget *status_image;
	GtkWidget *status_label;
	GtkWidget *page_stack;
	GtkWidget *prompt_label;
	GtkWidget *login_radio;
	GtkWidget *register_radio;
	GtkWidget *reset_radio;
	GtkWidget *email_entry;
	GtkWidget *password_entry;
	GtkWidget *remember_check;
	GtkWidget *passcode_entry;

	SoupSession *session;

	gboolean get_macaroon;

	GVariant *macaroon;
	gchar *consumer_key;
	gchar *consumer_secret;
	gchar *token_key;
	gchar *token_secret;
};

G_DEFINE_TYPE (GsUbuntuoneDialog, gs_ubuntuone_dialog, GTK_TYPE_DIALOG)

static gboolean
is_email_address (const gchar *text)
{
	text = g_utf8_strchr (text, -1, '@');

	if (!text)
		return FALSE;

	text = g_utf8_strchr (text + 1, -1, '.');

	if (!text)
		return FALSE;

	return text[1];
}

static void
update_widgets (GsUbuntuoneDialog *self)
{
	if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-0")) {
		gtk_widget_set_sensitive (self->next_button,
					  !gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->login_radio)) ||
					  (is_email_address (gtk_entry_get_text (GTK_ENTRY (self->email_entry))) &&
					   gtk_entry_get_text_length (GTK_ENTRY (self->password_entry)) > 0));
		gtk_widget_set_sensitive (self->password_entry,
					  gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->login_radio)));
		gtk_widget_set_sensitive (self->remember_check,
					  gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->login_radio)));
	} else if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-1")) {
		gtk_widget_set_sensitive (self->next_button, gtk_entry_get_text_length (GTK_ENTRY (self->passcode_entry)) > 0);
	} else if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-2")) {
		gtk_widget_set_visible (self->cancel_button, FALSE);
		gtk_widget_set_sensitive (self->cancel_button, FALSE);
		gtk_button_set_label (GTK_BUTTON (self->next_button), _("_Continue"));
	}
}

typedef void (*ResponseCallback) (GsUbuntuoneDialog *self,
				  guint	      status,
				  GVariant	  *response,
				  gpointer	   user_data);

typedef struct
{
	GsUbuntuoneDialog *dialog;
	ResponseCallback callback;
	gpointer user_data;
} RequestInfo;

static void
response_received_cb (SoupSession *session,
		      SoupMessage *message,
		      gpointer     user_data)
{
	RequestInfo *info = user_data;
	g_autoptr(GVariant) response = NULL;
	guint status;
	GBytes *bytes;
	g_autofree gchar *body = NULL;
	gsize length;

	g_object_get (message,
		      SOUP_MESSAGE_STATUS_CODE, &status,
		      SOUP_MESSAGE_RESPONSE_BODY_DATA, &bytes,
		      NULL);

	body = g_bytes_unref_to_data (bytes, &length);

	if (body)
		response = json_gvariant_deserialize_data (body, length, NULL, NULL);

	if (response)
		g_variant_ref_sink (response);

	if (info->callback)
		info->callback (info->dialog, status, response, info->user_data);

	g_free (info);
}

static void
send_request (GsUbuntuoneDialog *self,
	      const gchar         *method,
	      const gchar         *uri,
	      GVariant	          *request,
	      ResponseCallback     callback,
	      gpointer	           user_data)
{
	RequestInfo *info;
	SoupMessage *message;
	gchar *body;
	gsize length;
	g_autofree gchar *url = NULL;

	if (self->session == NULL)
		self->session = soup_session_new_with_options (SOUP_SESSION_USER_AGENT,
							       gs_user_agent (),
							       NULL);

	body = json_gvariant_serialize_data (g_variant_ref_sink (request), &length);
	g_variant_unref (request);

	url = g_strdup_printf ("%s%s", UBUNTU_LOGIN_HOST, uri);
	message = soup_message_new (method, url);

	info = g_new0 (RequestInfo, 1);
	info->dialog = self;
	info->callback = callback;
	info->user_data = user_data;

	soup_message_set_request (message, "application/json", SOUP_MEMORY_TAKE, body, length);
	soup_session_queue_message (self->session, message, response_received_cb, info);
}

static void
show_status (GsUbuntuoneDialog *self,
	     const gchar       *text,
	     gboolean           is_error)
{
	PangoAttrList *attributes;

	gtk_widget_set_visible (self->status_stack, TRUE);

	if (is_error) {
		gtk_stack_set_visible_child_name (GTK_STACK (self->status_stack), "status-image");
		gtk_image_set_from_icon_name (GTK_IMAGE (self->status_image), "gtk-dialog-error", GTK_ICON_SIZE_BUTTON);
	} else {
		gtk_stack_set_visible_child_name (GTK_STACK (self->status_stack), "status-spinner");
	}

	attributes = pango_attr_list_new ();
	pango_attr_list_insert (attributes, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
	pango_attr_list_insert (attributes, pango_attr_foreground_new (is_error ? 65535 : 0, 0, 0));
	gtk_label_set_attributes (GTK_LABEL (self->status_label), attributes);
	pango_attr_list_unref (attributes);

	gtk_label_set_text (GTK_LABEL (self->status_label), text);
}

static void
reenable_widgets (GsUbuntuoneDialog *self)
{
	gtk_label_set_text (GTK_LABEL (self->status_label), NULL);
	gtk_stack_set_visible_child_name (GTK_STACK (self->status_stack), "status-image");
	gtk_widget_set_visible (self->status_stack, FALSE);

	gtk_widget_set_sensitive (self->cancel_button, TRUE);
	gtk_widget_set_sensitive (self->next_button, TRUE);
	gtk_widget_set_sensitive (self->login_radio, TRUE);
	gtk_widget_set_sensitive (self->register_radio, TRUE);
	gtk_widget_set_sensitive (self->reset_radio, TRUE);
	gtk_widget_set_sensitive (self->email_entry, TRUE);
	gtk_widget_set_sensitive (self->password_entry, TRUE);
	gtk_widget_set_sensitive (self->remember_check, TRUE);
	gtk_widget_set_sensitive (self->passcode_entry, TRUE);
}

static void
receive_login_response_cb (GsUbuntuoneDialog *self,
			   guint	        status,
			   GVariant	       *response,
			   gpointer	        user_data)
{
	const gchar *code;

	reenable_widgets (self);

	if (response) {
		switch (status) {
		case SOUP_STATUS_OK:
		case SOUP_STATUS_CREATED:
			g_clear_pointer (&self->token_secret, g_free);
			g_clear_pointer (&self->token_key, g_free);
			g_clear_pointer (&self->consumer_secret, g_free);
			g_clear_pointer (&self->consumer_key, g_free);

			g_variant_lookup (response, "consumer_key", "s", &self->consumer_key);
			g_variant_lookup (response, "consumer_secret", "s", &self->consumer_secret);
			g_variant_lookup (response, "token_key", "s", &self->token_key);
			g_variant_lookup (response, "token_secret", "s", &self->token_secret);

			gtk_stack_set_visible_child_name (GTK_STACK (self->page_stack), "page-2");
			update_widgets (self);
			break;

		default:
			g_variant_lookup (response, "code", "&s", &code);

			if (!code)
				code = "";

			if (g_str_equal (code, "TWOFACTOR_REQUIRED")) {
				gtk_stack_set_visible_child_name (GTK_STACK (self->page_stack), "page-1");
				gtk_widget_grab_focus (self->passcode_entry);
				update_widgets (self);
				break;
			}

			update_widgets (self);

			if (g_str_equal (code, "INVALID_CREDENTIALS")) {
				show_status (self, _("Incorrect email or password"), TRUE);
				gtk_widget_grab_focus (self->password_entry);
			} else if (g_str_equal (code, "ACCOUNT_SUSPENDED")) {
				show_status (self, _("Account suspended"), TRUE);
				gtk_widget_grab_focus (self->email_entry);
			} else if (g_str_equal (code, "ACCOUNT_DEACTIVATED")) {
				show_status (self, _("Account deactivated"), TRUE);
				gtk_widget_grab_focus (self->email_entry);
			} else if (g_str_equal (code, "EMAIL_INVALIDATED")) {
				show_status (self, _("Email invalidated"), TRUE);
				gtk_widget_grab_focus (self->email_entry);
			} else if (g_str_equal (code, "TWOFACTOR_FAILURE")) {
				show_status (self, _("Two-factor authentication failed"), TRUE);
				gtk_widget_grab_focus (self->passcode_entry);
			} else if (g_str_equal (code, "PASSWORD_POLICY_ERROR")) {
				show_status (self, _("Password reset required"), TRUE);
				gtk_widget_grab_focus (self->reset_radio);
			} else if (g_str_equal (code, "TOO_MANY_REQUESTS")) {
				show_status (self, _("Too many requests"), TRUE);
				gtk_widget_grab_focus (self->password_entry);
			} else {
				show_status (self, _("An error occurred"), TRUE);
				gtk_widget_grab_focus (self->password_entry);
			}

			break;
		}
	} else {
		update_widgets (self);
		show_status (self, _("An error occurred"), TRUE);
		gtk_widget_grab_focus (self->password_entry);
	}
}

static void
send_login_request (GsUbuntuoneDialog *self)
{
	gtk_widget_set_sensitive (self->cancel_button, FALSE);
	gtk_widget_set_sensitive (self->next_button, FALSE);
	gtk_widget_set_sensitive (self->login_radio, FALSE);
	gtk_widget_set_sensitive (self->register_radio, FALSE);
	gtk_widget_set_sensitive (self->reset_radio, FALSE);
	gtk_widget_set_sensitive (self->email_entry, FALSE);
	gtk_widget_set_sensitive (self->password_entry, FALSE);
	gtk_widget_set_sensitive (self->remember_check, FALSE);
	gtk_widget_set_sensitive (self->passcode_entry, FALSE);

	show_status (self, _("Signing in…"), FALSE);

	if (self->get_macaroon) {
#ifdef USE_SNAPD
		const gchar *username, *password, *otp;
		g_autoptr(SnapdAuthData) auth_data = NULL;
		g_autoptr(GError) error = NULL;

		username = gtk_entry_get_text (GTK_ENTRY (self->email_entry));
		password = gtk_entry_get_text (GTK_ENTRY (self->password_entry));
		otp = gtk_entry_get_text (GTK_ENTRY (self->passcode_entry));
		if (otp[0] == '\0')
			otp = NULL;

		auth_data = snapd_login_sync (username, password, otp, NULL, &error);
		reenable_widgets (self);
		if (auth_data != NULL) {
			self->macaroon = g_variant_ref_sink (g_variant_new ("(s^as)", snapd_auth_data_get_macaroon (auth_data), snapd_auth_data_get_discharges (auth_data)));
			gtk_stack_set_visible_child_name (GTK_STACK (self->page_stack), "page-2");
			update_widgets (self);
		} else {
			if (g_error_matches (error, SNAPD_ERROR, SNAPD_ERROR_AUTH_DATA_INVALID) ||
			    g_error_matches (error, SNAPD_ERROR, SNAPD_ERROR_AUTH_DATA_REQUIRED)) {
				show_status (self, _("Incorrect email or password"), TRUE);
				gtk_widget_grab_focus (self->password_entry);
			} else if (g_error_matches (error, SNAPD_ERROR, SNAPD_ERROR_TWO_FACTOR_REQUIRED)) {
				gtk_stack_set_visible_child_name (GTK_STACK (self->page_stack), "page-1");
				gtk_widget_grab_focus (self->passcode_entry);
				update_widgets (self);
			} else if (g_error_matches (error, SNAPD_ERROR, SNAPD_ERROR_TWO_FACTOR_INVALID)) {
				show_status (self, _("Two-factor authentication failed"), TRUE);
				gtk_widget_grab_focus (self->passcode_entry);
			} else {
				show_status (self, _("An error occurred"), TRUE);
				gtk_widget_grab_focus (self->password_entry);
			}
		}
#endif
	} else {
		GVariant *request;

		if (gtk_entry_get_text_length (GTK_ENTRY (self->passcode_entry)) > 0) {
			request = g_variant_new_parsed ("{"
							"  'token_name' : <'GNOME Software'>,"
							"  'email' : <%s>,"
							"  'password' : <%s>,"
							"  'otp' : <%s>"
							"}",
							gtk_entry_get_text (GTK_ENTRY (self->email_entry)),
							gtk_entry_get_text (GTK_ENTRY (self->password_entry)),
							gtk_entry_get_text (GTK_ENTRY (self->passcode_entry)));
		} else {
			request = g_variant_new_parsed ("{"
							"  'token_name' : <'GNOME Software'>,"
							"  'email' : <%s>,"
							"  'password' : <%s>"
							"}",
							gtk_entry_get_text (GTK_ENTRY (self->email_entry)),
							gtk_entry_get_text (GTK_ENTRY (self->password_entry)));
		}

		send_request (self,
			      SOUP_METHOD_POST,
			      "/api/v2/tokens/oauth",
			      request,
			      receive_login_response_cb,
			      NULL);
	}
}

static void
next_button_clicked_cb (GsUbuntuoneDialog *self,
			GtkButton	    *button)
{
	if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-0")) {
		if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->login_radio))) {
			send_login_request (self);
		} else if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->register_radio))) {
			g_app_info_launch_default_for_uri ("https://login.ubuntu.com/+new_account", NULL, NULL);
		} else if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->reset_radio))) {
			g_app_info_launch_default_for_uri ("https://login.ubuntu.com/+forgot_password", NULL, NULL);
		}
	} else if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-1")) {
		send_login_request (self);
	} else if (g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (self->page_stack)), "page-2")) {
		gtk_dialog_response (GTK_DIALOG (self), GTK_RESPONSE_OK);
	}
}

static void
radio_button_toggled_cb (GsUbuntuoneDialog *self,
			 GtkToggleButton   *toggle)
{
	update_widgets (self);
}

static void
entry_edited_cb (GsUbuntuoneDialog *self,
		 GParamSpec	     *pspec,
		 GObject	     *object)
{
	update_widgets (self);
}

static void
gs_ubuntuone_dialog_init (GsUbuntuoneDialog *self)
{
	GList *focus_chain = NULL;

	gtk_widget_init_template (GTK_WIDGET (self));

	gtk_window_set_default (GTK_WINDOW (self), self->next_button);

	focus_chain = g_list_append (focus_chain, self->email_entry);
	focus_chain = g_list_append (focus_chain, self->password_entry);
	focus_chain = g_list_append (focus_chain, self->remember_check);
	focus_chain = g_list_append (focus_chain, self->login_radio);
	focus_chain = g_list_append (focus_chain, self->register_radio);
	focus_chain = g_list_append (focus_chain, self->reset_radio);
	gtk_container_set_focus_chain (GTK_CONTAINER (gtk_widget_get_parent (self->email_entry)), focus_chain);
	g_list_free (focus_chain);

	g_signal_connect_swapped (self->next_button, "clicked", G_CALLBACK (next_button_clicked_cb), self);
	g_signal_connect_swapped (self->login_radio, "toggled", G_CALLBACK (radio_button_toggled_cb), self);
	g_signal_connect_swapped (self->register_radio, "toggled", G_CALLBACK (radio_button_toggled_cb), self);
	g_signal_connect_swapped (self->reset_radio, "toggled", G_CALLBACK (radio_button_toggled_cb), self);
	g_signal_connect_swapped (self->email_entry, "notify::text", G_CALLBACK (entry_edited_cb), self);
	g_signal_connect_swapped (self->password_entry, "notify::text", G_CALLBACK (entry_edited_cb), self);
	g_signal_connect_swapped (self->passcode_entry, "notify::text", G_CALLBACK (entry_edited_cb), self);

	update_widgets (self);
}

static void
gs_ubuntuone_dialog_dispose (GObject *object)
{
	GsUbuntuoneDialog *self = GS_UBUNTUONE_DIALOG (object);

	g_clear_object (&self->session);

	G_OBJECT_CLASS (gs_ubuntuone_dialog_parent_class)->dispose (object);
}

static void
gs_ubuntuone_dialog_finalize (GObject *object)
{
	GsUbuntuoneDialog *self = GS_UBUNTUONE_DIALOG (object);

	g_clear_pointer (&self->token_secret, g_free);
	g_clear_pointer (&self->token_key, g_free);
	g_clear_pointer (&self->consumer_secret, g_free);
	g_clear_pointer (&self->consumer_key, g_free);
	g_clear_pointer (&self->macaroon, g_variant_unref);

	G_OBJECT_CLASS (gs_ubuntuone_dialog_parent_class)->finalize (object);
}

static void
gs_ubuntuone_dialog_class_init (GsUbuntuoneDialogClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->dispose = gs_ubuntuone_dialog_dispose;
	object_class->finalize = gs_ubuntuone_dialog_finalize;

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/plugins/gs-ubuntuone-dialog.ui");

	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, content_box);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, cancel_button);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, next_button);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, status_stack);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, status_image);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, status_label);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, page_stack);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, prompt_label);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, login_radio);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, register_radio);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, reset_radio);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, email_entry);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, password_entry);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, remember_check);
	gtk_widget_class_bind_template_child (widget_class, GsUbuntuoneDialog, passcode_entry);
}

gboolean
gs_ubuntuone_dialog_get_do_remember (GsUbuntuoneDialog *dialog)
{
	g_return_val_if_fail (GS_IS_UBUNTUONE_DIALOG (dialog), FALSE);
	return gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (dialog->remember_check));
}

GVariant *
gs_ubuntuone_dialog_get_macaroon (GsUbuntuoneDialog *dialog)
{
	g_return_val_if_fail (GS_IS_UBUNTUONE_DIALOG (dialog), NULL);
	return dialog->macaroon;
}

const gchar *
gs_ubuntuone_dialog_get_consumer_key (GsUbuntuoneDialog *dialog)
{
	g_return_val_if_fail (GS_IS_UBUNTUONE_DIALOG (dialog), NULL);
	return dialog->consumer_key;
}

const gchar *
gs_ubuntuone_dialog_get_consumer_secret (GsUbuntuoneDialog *dialog)
{
	g_return_val_if_fail (GS_IS_UBUNTUONE_DIALOG (dialog), NULL);
	return dialog->consumer_secret;
}

const gchar *
gs_ubuntuone_dialog_get_token_key (GsUbuntuoneDialog *dialog)
{
	g_return_val_if_fail (GS_IS_UBUNTUONE_DIALOG (dialog), NULL);
	return dialog->token_key;
}

const gchar *
gs_ubuntuone_dialog_get_token_secret (GsUbuntuoneDialog *dialog)
{
	g_return_val_if_fail (GS_IS_UBUNTUONE_DIALOG (dialog), NULL);
	return dialog->token_secret;
}

GtkWidget *
gs_ubuntuone_dialog_new (gboolean get_macaroon)
{
	GsUbuntuoneDialog *dialog = g_object_new (GS_TYPE_UBUNTUONE_DIALOG,
						  "use-header-bar", TRUE,
						  NULL);

	dialog->get_macaroon = get_macaroon;

	if (dialog->get_macaroon)
		gtk_label_set_label (GTK_LABEL (dialog->prompt_label),
			_("To install and remove snaps, you need an Ubuntu Single Sign-On account."));
	else
		gtk_label_set_label (GTK_LABEL (dialog->prompt_label),
			_("To rate and review software, you need an Ubuntu Single Sign-On account."));

	return GTK_WIDGET (dialog);
}

/* vim: set noexpandtab: */
