// SPDX-License-Identifier: GPL-2.0+
/* NetworkManager Applet -- allow user control over networking
 *
 * Dan Williams <dcbw@redhat.com>
 *
 * Copyright 2008 - 2017 Red Hat, Inc.
 * Copyright 2008 Novell, Inc.
 */

#include "nm-default.h"

#include "applet.h"
#include "applet-device-ethernet.h"
#include "ethernet-dialog.h"

#define DEFAULT_ETHERNET_NAME _("Auto Ethernet")

static gboolean
ethernet_new_auto_connection (NMDevice *device,
                              gpointer dclass_data,
                              AppletNewAutoConnectionCallback callback,
                              gpointer callback_data)
{
	NMConnection *connection;
	NMSettingWired *s_wired = NULL;
	NMSettingConnection *s_con;
	char *uuid;

	connection = nm_simple_connection_new ();

	s_wired = NM_SETTING_WIRED (nm_setting_wired_new ());
	nm_connection_add_setting (connection, NM_SETTING (s_wired));

	s_con = NM_SETTING_CONNECTION (nm_setting_connection_new ());
	uuid = nm_utils_uuid_generate ();
	g_object_set (s_con,
	              NM_SETTING_CONNECTION_ID, DEFAULT_ETHERNET_NAME,
	              NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRED_SETTING_NAME,
	              NM_SETTING_CONNECTION_AUTOCONNECT, TRUE,
	              NM_SETTING_CONNECTION_UUID, uuid,
	              NULL);
	g_free (uuid);

	nm_connection_add_setting (connection, NM_SETTING (s_con));

	(*callback) (connection, TRUE, FALSE, callback_data);
	return TRUE;
}

static gboolean
ethernet_add_menu_item (NMDevice *device,
                        gboolean multiple_devices,
                        const GPtrArray *connections,
                        NMConnection *active,
                        GtkWidget *menu,
                        NMApplet *applet)
{
	char *text;
	GtkWidget *item;
	gboolean carrier = TRUE;

	if (nm_device_get_state (device) == NM_DEVICE_STATE_UNMANAGED) {
		return FALSE;
	}

	if (multiple_devices) {
		const char *desc;

		desc = nm_device_get_description (device);

		if (connections->len > 1)
			text = g_strdup_printf (_("Ethernet Networks (%s)"), desc);
		else
			text = g_strdup_printf (_("Ethernet Network (%s)"), desc);
	} else {
		if (connections->len > 1)
			text = g_strdup (_("Ethernet Networks"));
		else
			text = g_strdup (_("Ethernet Network"));
	}

	item = applet_menu_item_create_device_item_helper (device, applet, text);
	g_free (text);

	/* Only dim the item if the device supports carrier detection AND
	 * we know it doesn't have a link.
	 */
 	if (nm_device_get_capabilities (device) & NM_DEVICE_CAP_CARRIER_DETECT)
		carrier = nm_device_ethernet_get_carrier (NM_DEVICE_ETHERNET (device));

	gtk_widget_set_sensitive (item, FALSE);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	gtk_widget_show (item);

	if (connections->len)
		applet_add_connection_items (device, connections, carrier, active, NMA_ADD_ACTIVE, menu, applet);

	/* Notify user of unmanaged or unavailable device */
	item = nma_menu_device_get_menu_item (device, applet, carrier ? NULL : _("disconnected"));
	if (item) {
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
		gtk_widget_show (item);
	}

	if (!nma_menu_device_check_unusable (device)) {
		if ((!active && connections->len) || (active && connections->len > 1))
			applet_menu_item_add_complex_separator_helper (menu, applet, _("Available"));

		if (connections->len)
			applet_add_connection_items (device, connections, carrier, active, NMA_ADD_INACTIVE, menu, applet);
		else
			applet_add_default_connection_item (device, DEFAULT_ETHERNET_NAME, carrier, menu, applet);
	}

	return TRUE;
}

static void
ethernet_notify_connected (NMDevice *device,
                           const char *msg,
                           NMApplet *applet)
{
	applet_do_notify (applet,
	                  _("Connection Established"),
	                  msg ? msg : _("You are now connected to the ethernet network."),
	                  "nm-device-wired",
	                  PREF_DISABLE_CONNECTED_NOTIFICATIONS);
}

static void
ethernet_get_icon (NMDevice *device,
                   NMDeviceState state,
                   NMConnection *connection,
                   GdkPixbuf **out_pixbuf,
                   const char **out_icon_name,
                   char **tip,
                   NMApplet *applet)
{
	NMSettingConnection *s_con;
	const char *id;

	g_return_if_fail (out_icon_name && !*out_icon_name);
	g_return_if_fail (tip && !*tip);

	id = nm_device_get_iface (NM_DEVICE (device));
	if (connection) {
		s_con = nm_connection_get_setting_connection (connection);
		id = nm_setting_connection_get_id (s_con);
	}

	switch (state) {
	case NM_DEVICE_STATE_PREPARE:
		*tip = g_strdup_printf (_("Preparing ethernet network connection “%s”…"), id);
		break;
	case NM_DEVICE_STATE_CONFIG:
		*tip = g_strdup_printf (_("Configuring ethernet network connection “%s”…"), id);
		break;
	case NM_DEVICE_STATE_NEED_AUTH:
		*tip = g_strdup_printf (_("User authentication required for ethernet network connection “%s”…"), id);
		break;
	case NM_DEVICE_STATE_IP_CONFIG:
		*tip = g_strdup_printf (_("Requesting an ethernet network address for “%s”…"), id);
		break;
	case NM_DEVICE_STATE_ACTIVATED:
		*out_icon_name = "nm-device-wired";
		*tip = g_strdup_printf (_("Ethernet network connection “%s” active"), id);
		break;
	default:
		break;
	}
}

/* PPPoE */

typedef struct {
	SecretsRequest req;

	GtkWidget *dialog;
	GtkEntry *username_entry;
	GtkEntry *service_entry;
	GtkEntry *password_entry;
	GtkWidget *ok_button;
} NMPppoeInfo;

static void
pppoe_verify (GtkEditable *editable, gpointer user_data)
{
	NMPppoeInfo *info = (NMPppoeInfo *) user_data;
	const char *s;
	gboolean valid = TRUE;

	s = gtk_entry_get_text (info->username_entry);
	if (!s || strlen (s) < 1)
		valid = FALSE;

	if (valid) {
		s = gtk_entry_get_text (info->password_entry);
		if (!s || strlen (s) < 1)
			valid = FALSE;
	}

	gtk_widget_set_sensitive (info->ok_button, valid);
}

static void
pppoe_update_setting (NMSettingPppoe *pppoe, NMPppoeInfo *info)
{
	const char *s;

	s = gtk_entry_get_text (info->service_entry);
	if (s && strlen (s) < 1)
		s = NULL;

	g_object_set (pppoe,
	              NM_SETTING_PPPOE_USERNAME, gtk_entry_get_text (info->username_entry),
	              NM_SETTING_PPPOE_PASSWORD, gtk_entry_get_text (info->password_entry),
	              NM_SETTING_PPPOE_SERVICE, s,
	              NULL);
}

typedef enum { 
    USERNAME, PSSWD, ALL
} show_only_t;

static void
pppoe_hide_secret_dialog_fields (GtkBuilder* builder, show_only_t show_field)
{	
	GtkWidget *w;

	switch (show_field)
	{
	case USERNAME:
		w = GTK_WIDGET (gtk_builder_get_object (builder, "dsl_show_password"));
		gtk_widget_set_visible(w, FALSE);

		w = GTK_WIDGET (gtk_builder_get_object (builder, "label25"));
		gtk_widget_set_visible(w, FALSE);

		w = GTK_WIDGET (gtk_builder_get_object (builder, "dsl_password"));
		gtk_widget_set_visible(w, FALSE);
		
		break;
	case PSSWD:
	{
		w = GTK_WIDGET (gtk_builder_get_object (builder, "dsl_username"));
		gtk_widget_set_visible(w, FALSE);

		w = GTK_WIDGET (gtk_builder_get_object (builder, "label24"));
		gtk_widget_set_visible(w, FALSE);

		break;
	}
	case ALL:
	{
		w = GTK_WIDGET (gtk_builder_get_object (builder, "dsl_ask_user_data"));
		gtk_widget_set_visible(w, FALSE);

		return;
	}
	default:
		break;
	}

	w = GTK_WIDGET(gtk_builder_get_object (builder, "dsl_ask_user_data"));
	gtk_widget_set_visible(w, FALSE);

	w = GTK_WIDGET(gtk_builder_get_object (builder, "dsl_interface_label"));
	gtk_widget_set_visible(w, FALSE);

	w = GTK_WIDGET(gtk_builder_get_object (builder, "dsl_interface"));
	gtk_widget_set_visible(w, FALSE);

	w = GTK_WIDGET(gtk_builder_get_object (builder, "dsl_parent"));
	gtk_widget_set_visible(w, FALSE);

	w = GTK_WIDGET(gtk_builder_get_object (builder, "parent_interface_label"));
	gtk_widget_set_visible(w, FALSE);

	w = GTK_WIDGET(gtk_builder_get_object (builder, "dsl_claim_button"));
	gtk_widget_set_visible(w, FALSE);

	w = GTK_WIDGET(gtk_builder_get_object (builder, "label26"));
	gtk_widget_set_visible(w, FALSE);

	w = GTK_WIDGET(gtk_builder_get_object (builder, "dsl_service"));
	gtk_widget_set_visible(w, FALSE);
}

static void
pppoe_update_ui (NMConnection *connection, NMPppoeInfo *info, GtkBuilder* builder)
{
	NMSettingPppoe *s_pppoe;
	const char *s;

	g_return_if_fail (NM_IS_CONNECTION (connection));
	g_return_if_fail (info != NULL);

	s_pppoe = nm_connection_get_setting_pppoe (connection);
	g_return_if_fail (s_pppoe != NULL);

	s = nm_setting_pppoe_get_username (s_pppoe);
	if (s)
		gtk_entry_set_text (info->username_entry, s);

	s = nm_setting_pppoe_get_service (s_pppoe);
	if (s)
		gtk_entry_set_text (info->service_entry, s);

	s = nm_setting_pppoe_get_password (s_pppoe);
	if (s)
		gtk_entry_set_text (info->password_entry, s);
}

static void
free_pppoe_info (SecretsRequest *req)
{
	NMPppoeInfo *info = (NMPppoeInfo *) req;

	if (info->dialog) {
		gtk_widget_hide (info->dialog);
		gtk_widget_destroy (info->dialog);
	}
}

static void
get_pppoe_secrets_cb (GtkDialog *dialog, gint response, gpointer user_data)
{
	SecretsRequest *req = user_data;
	NMPppoeInfo *info = (NMPppoeInfo *) req;
	NMSettingPppoe *setting;
	GVariant *secrets = NULL;
	GError *error = NULL;

	if (response != GTK_RESPONSE_OK) {
		g_set_error (&error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_USER_CANCELED,
		             "%s.%d (%s): canceled",
		             __FILE__, __LINE__, __func__);
		goto done;
	}

	setting = nm_connection_get_setting_pppoe (req->connection);
	pppoe_update_setting (setting, info);

	secrets = nm_connection_to_dbus (req->connection, NM_CONNECTION_SERIALIZE_ONLY_SECRETS);
	if (!secrets) {
		g_set_error (&error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_FAILED,
					 "%s.%d (%s): failed to hash setting " NM_SETTING_PPPOE_SETTING_NAME,
					 __FILE__, __LINE__, __func__);
	}

done:
	applet_secrets_request_complete (req, secrets, error);
	applet_secrets_request_free (req);

	if (secrets)
		g_variant_unref (secrets);
}

static void
show_password_toggled (GtkToggleButton *button, gpointer user_data)
{
	NMPppoeInfo *info = (NMPppoeInfo *) user_data;

	if (gtk_toggle_button_get_active (button))
		gtk_entry_set_visibility (GTK_ENTRY (info->password_entry), TRUE);
	else
		gtk_entry_set_visibility (GTK_ENTRY (info->password_entry), FALSE);
}

static gboolean
pppoe_fill_dialog(GtkBuilder **out_builder, 
				  GtkWidget **out_dialog, 
				  NMConnection *connection, 
				  GError **error,
				  show_only_t show_field)
{
	NMSettingPppoe *s_pppoe;
	GError *tmp_error = NULL;
	GtkBuilder *builder;
	GtkWidget *dialog;

	g_return_val_if_fail (out_builder != NULL, FALSE);
	g_return_val_if_fail (out_dialog != NULL, FALSE);
	g_return_val_if_fail (NM_IS_CONNECTION (connection), FALSE);

	builder = gtk_builder_new ();
	if (!gtk_builder_add_from_resource (builder, "/org/freedesktop/network-manager-applet/connection-editor/ce-page-dsl.ui", &tmp_error)) {
		g_set_error (error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_FAILED,
					 "%s.%d (%s): couldn't display secrets UI: %s",
		             __FILE__, __LINE__, __func__, tmp_error->message);
		g_error_free (tmp_error);
		g_object_unref (builder);
		return FALSE;
	}

	dialog = gtk_dialog_new ();
	gtk_window_set_title (GTK_WINDOW (dialog), _("DSL authentication"));
	gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);

	s_pppoe = nm_connection_get_setting_pppoe (connection);
	g_return_val_if_fail (s_pppoe, FALSE);

	if (nm_setting_pppoe_get_password_flags (s_pppoe) == NM_SETTING_SECRET_FLAG_NOT_SAVED) {
		pppoe_hide_secret_dialog_fields(builder, show_field);
	}
	else {
		pppoe_hide_secret_dialog_fields(builder, ALL);
	}

	*out_builder = builder;
	*out_dialog = dialog;

	return TRUE;
}

static gboolean
pppoe_get_secrets (SecretsRequest *req, GError **error)
{
	NMPppoeInfo *info = (NMPppoeInfo *) req;
	GtkWidget *w;
	GtkBuilder *builder = NULL;

	if (!pppoe_fill_dialog (&builder, 
		&info->dialog, 
		req->connection, 
		error, 
		PSSWD)) {
		return FALSE;
	}

	applet_secrets_request_set_free_func (req, free_pppoe_info);

	info->username_entry = GTK_ENTRY (gtk_builder_get_object (builder, "dsl_username"));
	g_signal_connect (info->username_entry, "changed", G_CALLBACK (pppoe_verify), info);

	info->service_entry = GTK_ENTRY (gtk_builder_get_object (builder, "dsl_service"));

	info->password_entry = GTK_ENTRY (gtk_builder_get_object (builder, "dsl_password"));
	g_signal_connect (info->password_entry, "changed", G_CALLBACK (pppoe_verify), info);

	gtk_dialog_add_button (GTK_DIALOG (info->dialog), _("_Cancel"), GTK_RESPONSE_REJECT);
	w = gtk_dialog_add_button (GTK_DIALOG (info->dialog), _("_OK"), GTK_RESPONSE_OK);
	info->ok_button = w;

	gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (info->dialog))),
	                    GTK_WIDGET (gtk_builder_get_object (builder, "DslPage")),
	                    TRUE, TRUE, 0);

	pppoe_update_ui (req->connection, info, builder);

	w = GTK_WIDGET (gtk_builder_get_object (builder, "dsl_show_password"));
	g_signal_connect (w, "toggled", G_CALLBACK (show_password_toggled), info);

	g_signal_connect (info->dialog, "response", G_CALLBACK (get_pppoe_secrets_cb), info);

	gtk_window_set_position (GTK_WINDOW (info->dialog), GTK_WIN_POS_CENTER_ALWAYS);
	gtk_widget_realize (info->dialog);
	gtk_window_present (GTK_WINDOW (info->dialog));

	return TRUE;
}

/* 802.1x */

typedef struct {
	SecretsRequest req;
	GtkWidget *dialog;
} NM8021xInfo;

static void
free_8021x_info (SecretsRequest *req)
{
	NM8021xInfo *info = (NM8021xInfo *) req;

	if (info->dialog) {
		gtk_widget_hide (info->dialog);
		gtk_widget_destroy (info->dialog);
	}
}

static void
get_8021x_secrets_cb (GtkDialog *dialog, gint response, gpointer user_data)
{
	SecretsRequest *req = user_data;
	NM8021xInfo *info = (NM8021xInfo *) req;
	NMConnection *connection = NULL;
	NMSetting *setting;
	GError *error = NULL;

	if (response != GTK_RESPONSE_OK) {
		g_set_error (&error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_USER_CANCELED,
		             "%s.%d (%s): canceled",
		             __FILE__, __LINE__, __func__);
		goto done;
	}

	connection = nma_ethernet_dialog_get_connection (info->dialog);
	if (!connection) {
		g_set_error (&error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_FAILED,
		             "%s.%d (%s): couldn't get connection from ethernet dialog.",
		             __FILE__, __LINE__, __func__);
		goto done;
	}

	setting = nm_connection_get_setting (connection, NM_TYPE_SETTING_802_1X);
	if (setting) {
		nm_connection_add_setting (req->connection, g_object_ref (setting));
	} else {
		g_set_error (&error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_FAILED,
					 "%s.%d (%s): requested setting '802-1x' didn't"
					 " exist in the connection.",
					 __FILE__, __LINE__, __func__);
	}

done:
	applet_secrets_request_complete_setting (req, NM_SETTING_802_1X_SETTING_NAME, error);
	applet_secrets_request_free (req);
	g_clear_error (&error);
}

static gboolean
nm_8021x_get_secrets (SecretsRequest *req, GError **error)
{
	NM8021xInfo *info = (NM8021xInfo *) req;

	applet_secrets_request_set_free_func (req, free_8021x_info);

	info->dialog = nma_ethernet_dialog_new (g_object_ref (req->connection));
	if (!info->dialog) {
		g_set_error (error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_FAILED,
		             "%s.%d (%s): couldn't display secrets UI",
		             __FILE__, __LINE__, __func__);
		return FALSE;
	}

	g_signal_connect (info->dialog, "response", G_CALLBACK (get_8021x_secrets_cb), info);

	gtk_window_set_position (GTK_WINDOW (info->dialog), GTK_WIN_POS_CENTER_ALWAYS);
	gtk_widget_realize (info->dialog);
	gtk_window_present (GTK_WINDOW (info->dialog));

	return TRUE;
}

static gboolean
ethernet_get_secrets (SecretsRequest *req, GError **error)
{
	NMSettingConnection *s_con;
	const char *ctype;

	s_con = nm_connection_get_setting_connection (req->connection);
	if (!s_con) {
		g_set_error (error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_INVALID_CONNECTION,
		             "%s.%d (%s): Invalid connection",
		             __FILE__, __LINE__, __func__);
		return FALSE;
	}

	ctype = nm_setting_connection_get_connection_type (s_con);
	if (!strcmp (ctype, NM_SETTING_WIRED_SETTING_NAME))
		return nm_8021x_get_secrets (req, error);
	else if (!strcmp (ctype, NM_SETTING_PPPOE_SETTING_NAME))
		return pppoe_get_secrets (req, error);
	else {
		g_set_error (error,
		             NM_SECRET_AGENT_ERROR,
		             NM_SECRET_AGENT_ERROR_FAILED,
		             "%s.%d (%s): unhandled ethernet connection type '%s'",
		             __FILE__, __LINE__, __func__, ctype);
	}

	return FALSE;
}

/********************************************************************/
/* PPPoE pre-activation dialog support */
/********************************************************************/

typedef struct {
	NMApplet *applet;
	NMConnection *connection;
	NMDevice *device;
	char *specific_object;

	GtkWidget *dialog;
	GtkEntry *username_entry;
	GtkWidget *ok_button;

	void (*activate_connection_cb);
} PppoeActivateContext;

static void
pppoe_activate_context_free (PppoeActivateContext *ctx)
{
	if (ctx->dialog)
		gtk_widget_destroy (ctx->dialog);
	if (ctx->connection)
		g_object_unref (ctx->connection);
	if (ctx->device)
		g_object_unref (ctx->device);
	g_free (ctx->specific_object);
	g_free (ctx);
}

static void
pppoe_activate_verify (GtkEditable *editable, gpointer user_data)
{
	PppoeActivateContext *ctx = (PppoeActivateContext *) user_data;
	const char *username;
	gboolean valid = FALSE;

	username = gtk_entry_get_text (ctx->username_entry);

	if (username && strlen (username) > 0)
		valid = TRUE;

	gtk_widget_set_sensitive (ctx->ok_button, valid);
}

static void
pppoe_commit_changes_cb (GObject *connection,
                         GAsyncResult *result,
                         gpointer user_data)
{
	PppoeActivateContext *ctx = (PppoeActivateContext *) user_data;
	GError *error = NULL;

	if (!nm_remote_connection_commit_changes_finish (NM_REMOTE_CONNECTION (connection), result, &error)) {
		const char *err_text = error ? error->message : _("Unknown error");

		g_warning ("Failed to commit PPPoE connection changes: %s", err_text);
		g_clear_error (&error);
		pppoe_activate_context_free (ctx);
		return;
	}

	/* Connection updated successfully, now activate it */
	nm_client_activate_connection_async (ctx->applet->nm_client,
	                                     ctx->connection,
	                                     ctx->device,
	                                     ctx->specific_object,
	                                     NULL,
	                                     ctx->activate_connection_cb,
	                                     ctx->applet);

	pppoe_activate_context_free (ctx);
}

static void
pppoe_activate_dialog_response_cb (GtkDialog *dialog, gint response, gpointer user_data)
{
	PppoeActivateContext *ctx = (PppoeActivateContext *) user_data;
	NMSettingPppoe *s_pppoe;
	const char *username;

	if (response != GTK_RESPONSE_OK) {
		pppoe_activate_context_free (ctx);
		return;
	}

	/* Get username from dialog */
	username = gtk_entry_get_text (ctx->username_entry);

	/* Update the connection's pppoe setting */
	s_pppoe = nm_connection_get_setting_pppoe (ctx->connection);
	if (!s_pppoe) {
		g_warning ("PPPoE setting not found in connection");
		pppoe_activate_context_free (ctx);
		return;
	}

	g_object_set (s_pppoe,
	              NM_SETTING_PPPOE_USERNAME, username,
	              NULL);

	/* Commit changes to NetworkManager */
	nm_remote_connection_commit_changes_async (NM_REMOTE_CONNECTION (ctx->connection),
	                                           TRUE,
	                                           NULL,
	                                           pppoe_commit_changes_cb,
	                                           ctx);
}

static void
show_pppoe_activate_dialog (NMApplet *applet,
                             NMConnection *connection,
                             NMDevice *device,
                             const char *specific_object,
							 void (*activate_connection_cb))
{
	GtkBuilder *builder = NULL;
	PppoeActivateContext *ctx;

	ctx = g_new0 (PppoeActivateContext, 1);
	ctx->applet = applet;
	ctx->connection = g_object_ref (connection);
	ctx->device = device ? g_object_ref (device) : NULL;
	ctx->specific_object = g_strdup (specific_object);
	ctx->activate_connection_cb = activate_connection_cb;

	if (!pppoe_fill_dialog (&builder, 
		&ctx->dialog, 
		ctx->connection, 
		NULL,
		USERNAME)) {
		return;
	}

	gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (ctx->dialog))),
                      GTK_WIDGET (gtk_builder_get_object (builder, "DslPage")),
                      TRUE, TRUE, 0);

	g_signal_connect (ctx->dialog, "response", G_CALLBACK (pppoe_activate_dialog_response_cb), ctx);

	gtk_dialog_add_button (GTK_DIALOG (ctx->dialog), _("_Cancel"), GTK_RESPONSE_REJECT);
	ctx->ok_button = gtk_dialog_add_button (GTK_DIALOG (ctx->dialog), _("_OK"), GTK_RESPONSE_OK);

	ctx->username_entry = GTK_ENTRY (gtk_builder_get_object (builder, "dsl_username"));
	g_signal_connect (ctx->username_entry, "changed", G_CALLBACK (pppoe_activate_verify), ctx);

	pppoe_activate_verify (NULL, ctx);

	gtk_window_set_position (GTK_WINDOW (ctx->dialog), GTK_WIN_POS_CENTER_ALWAYS);
	gtk_widget_realize (ctx->dialog);
	gtk_window_present (GTK_WINDOW (ctx->dialog));
}

static gboolean
ethernet_get_auth_data(NMApplet *applet,
                       NMConnection *connection,
                       NMDevice *device,
                       const char *specific_object,
					   void (*activate_connection_cb))
{
	show_pppoe_activate_dialog (applet, 
								connection, 
								device,
								specific_object, 
								activate_connection_cb);
	return TRUE;
}

NMADeviceClass *
applet_device_ethernet_get_class (NMApplet *applet)
{
	NMADeviceClass *dclass;

	dclass = g_slice_new0 (NMADeviceClass);
	if (!dclass)
		return NULL;

	dclass->new_auto_connection = ethernet_new_auto_connection;
	dclass->add_menu_item = ethernet_add_menu_item;
	dclass->notify_connected = ethernet_notify_connected;
	dclass->get_icon = ethernet_get_icon;
	dclass->get_secrets = ethernet_get_secrets;
	dclass->secrets_request_size = MAX (sizeof (NM8021xInfo), sizeof (NMPppoeInfo));
	dclass->get_auth_data = ethernet_get_auth_data;

	return dclass;
}
