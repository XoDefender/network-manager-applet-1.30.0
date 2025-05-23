// SPDX-License-Identifier: GPL-2.0+
/* NetworkManager Applet -- allow user control over networking
 *
 * Copyright (C) 2004 - 2017 Red Hat, Inc.
 * Copyright (C) 2005 - 2008 Novell, Inc.
 *
 * This applet used the GNOME Wireless Applet as a skeleton to build from.
 *
 * GNOME Wireless Applet Authors:
 *		Eskil Heyn Olsen <eskil@eskil.dk>
 *		Bastien Nocera <hadess@hadess.net> (Gnome2 port)
 *
 * Copyright 2001, 2002 Free Software Foundation
 */

#include "nm-default.h"

#include <time.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdlib.h>

#include "applet.h"
#include "applet-device-bt.h"
#include "applet-device-ethernet.h"
#include "applet-device-wifi.h"
#include "applet-dialogs.h"
#include "nma-wifi-dialog.h"
#include "applet-vpn-request.h"
#include "utils.h"

#if WITH_WWAN
# include "applet-device-broadband.h"
#endif

#define NOTIFY_CAPS_ACTIONS_KEY "actions"

extern gboolean shell_debug;
extern gboolean with_agent;
extern gboolean with_appindicator;

G_DEFINE_TYPE (NMApplet, nma, G_TYPE_APPLICATION)

/********************************************************************/

static gboolean
applet_request_wifi_scan (NMApplet *applet)
{
	const GPtrArray *devices;
	NMDevice *device;
	int i;

	g_debug ("requesting wifi scan");

	/* Request scan for all wifi devices */
	devices = nm_client_get_devices (applet->nm_client);
	for (i = 0; devices && i < devices->len; i++) {
		device = g_ptr_array_index (devices, i);
		if (NM_IS_DEVICE_WIFI (device))
			nm_device_wifi_request_scan ((NMDeviceWifi *) device, NULL, NULL);
	}

	return G_SOURCE_CONTINUE;
}

static void
applet_start_wifi_scan (NMApplet *applet, gpointer unused)
{
	nm_clear_g_source (&applet->wifi_scan_id);
	applet->wifi_scan_id = g_timeout_add_seconds (15,
	                                              (GSourceFunc) applet_request_wifi_scan,
	                                              applet);
	applet_request_wifi_scan (applet);
}

static void
applet_stop_wifi_scan (NMApplet *applet, gpointer unused)
{
	nm_clear_g_source (&applet->wifi_scan_id);
}

#ifdef WITH_APPINDICATOR
/* Work around ubuntu libappindicator not emitting the "show"/"hide" signals,
 * see https://bugs.launchpad.net/ubuntu/+source/libappindicator/+bug/522152.
 * There will be no periodic Wi-Fi access point updates but at least there
 * will be one each time the menu is opened.
 */
static void
applet_menu_about_to_show_cb (NMApplet *applet, gpointer unused)
{
	applet_request_wifi_scan (applet);
}

static void
applet_workaround_show_cb (NMApplet *applet, gpointer unused)
{
	GObject *menu_server;
	GObject *root_menu_item;

	g_object_get (applet->app_indicator, "dbus-menu-server", &menu_server, NULL);
	g_object_get (menu_server, "root-node", &root_menu_item, NULL);

	/* If libappindicator doesn't emit "show" and "hide" signals when the
	 * menu is opened/closed, there will be exactly one "show" signal when
	 * the menu is constructed.  Subscribe to DBusmenuMenuItem's
	 * "about-to-show" signal to work around this.  If we're receiving the
	 * "show" signal for the second time the workaround wasn't needed and
	 * we can undo it.
	 */
	if (!applet->app_indicator_show_signal_received) {
		applet->app_indicator_show_signal_received = TRUE;
		g_signal_connect_swapped (root_menu_item, "about-to-show", G_CALLBACK (applet_menu_about_to_show_cb), applet);
	} else {
		GtkMenu *menu = app_indicator_get_menu (applet->app_indicator);

		g_signal_handlers_disconnect_by_func (root_menu_item, applet_menu_about_to_show_cb, applet);
		g_signal_handlers_disconnect_by_func (menu, applet_workaround_show_cb, applet);
	}
}
#endif

static inline NMADeviceClass *
get_device_class (NMDevice *device, NMApplet *applet)
{
	g_return_val_if_fail (device != NULL, NULL);
	g_return_val_if_fail (applet != NULL, NULL);

	if (NM_IS_DEVICE_ETHERNET (device))
		return applet->ethernet_class;
	else if (NM_IS_DEVICE_WIFI (device))
		return applet->wifi_class;
	else if (NM_IS_DEVICE_MODEM (device)) {
#if WITH_WWAN
		return applet->broadband_class;
#else
		g_debug ("%s: modem found but WWAN support not enabled", __func__);
#endif
	} else if (NM_IS_DEVICE_BT (device))
		return applet->bt_class;
	else
		g_debug ("%s: Unknown device type '%s'", __func__, G_OBJECT_TYPE_NAME (device));
	return NULL;
}

static inline NMADeviceClass *
get_device_class_from_connection (NMConnection *connection, NMApplet *applet)
{
	NMSettingConnection *s_con;
	const char *ctype;

	g_return_val_if_fail (connection != NULL, NULL);
	g_return_val_if_fail (applet != NULL, NULL);

	s_con = nm_connection_get_setting_connection (connection);
	g_return_val_if_fail (s_con != NULL, NULL);

	ctype = nm_setting_connection_get_connection_type (s_con);
	g_return_val_if_fail (ctype != NULL, NULL);

	if (!strcmp (ctype, NM_SETTING_WIRED_SETTING_NAME) || !strcmp (ctype, NM_SETTING_PPPOE_SETTING_NAME))
		return applet->ethernet_class;
	else if (!strcmp (ctype, NM_SETTING_WIRELESS_SETTING_NAME))
		return applet->wifi_class;
#if WITH_WWAN
	else if (!strcmp (ctype, NM_SETTING_GSM_SETTING_NAME) || !strcmp (ctype, NM_SETTING_CDMA_SETTING_NAME))
		return applet->broadband_class;
#endif
	else if (!strcmp (ctype, NM_SETTING_BLUETOOTH_SETTING_NAME))
		return applet->bt_class;
	else
		g_warning ("%s: unhandled connection type '%s'", __func__, ctype);
	return NULL;
}

static NMActiveConnection *
applet_get_best_activating_connection (NMApplet *applet, NMDevice **device)
{
	NMActiveConnection *best = NULL;
	NMDevice *best_dev = NULL;
	const GPtrArray *connections;
	int i;

	g_return_val_if_fail (NM_IS_APPLET (applet), NULL);
	g_return_val_if_fail (device != NULL, NULL);
	g_return_val_if_fail (*device == NULL, NULL);

	connections = nm_client_get_active_connections (applet->nm_client);
	for (i = 0; connections && (i < connections->len); i++) {
		NMActiveConnection *candidate = g_ptr_array_index (connections, i);
		const GPtrArray *devices;
		NMDevice *candidate_dev;

		if (nm_active_connection_get_state (candidate) != NM_ACTIVE_CONNECTION_STATE_ACTIVATING)
			continue;

		devices = nm_active_connection_get_devices (candidate);
		if (!devices || !devices->len)
			continue;

		candidate_dev = g_ptr_array_index (devices, 0);
		if (!get_device_class (candidate_dev, applet))
			continue;

		if (!best_dev) {
			best_dev = candidate_dev;
			best = candidate;
			continue;
		}

		if (NM_IS_DEVICE_WIFI (best_dev)) {
			if (NM_IS_DEVICE_ETHERNET (candidate_dev)) {
				best_dev = candidate_dev;
				best = candidate;
			}
		} else if (NM_IS_DEVICE_MODEM (best_dev)) {
			NMDeviceModemCapabilities best_caps;
			NMDeviceModemCapabilities candidate_caps = NM_DEVICE_MODEM_CAPABILITY_NONE;

			best_caps = nm_device_modem_get_current_capabilities (NM_DEVICE_MODEM (best_dev));
			if (NM_IS_DEVICE_MODEM (candidate_dev))
				candidate_caps = nm_device_modem_get_current_capabilities (NM_DEVICE_MODEM (candidate_dev));

			if (best_caps & NM_DEVICE_MODEM_CAPABILITY_CDMA_EVDO) {
				if (   NM_IS_DEVICE_ETHERNET (candidate_dev)
				    || NM_IS_DEVICE_WIFI (candidate_dev)) {
					best_dev = candidate_dev;
					best = candidate;
				}
			} else if (best_caps & NM_DEVICE_MODEM_CAPABILITY_GSM_UMTS) {
				if (   NM_IS_DEVICE_ETHERNET (candidate_dev)
					|| NM_IS_DEVICE_WIFI (candidate_dev)
					|| (candidate_caps & NM_DEVICE_MODEM_CAPABILITY_CDMA_EVDO)) {
					best_dev = candidate_dev;
					best = candidate;
				}
			}
		}
	}

	*device = best_dev;
	return best;
}

static NMActiveConnection *
applet_get_default_active_connection (NMApplet *applet, NMDevice **device,
                                      gboolean only_known_devices)
{
	NMActiveConnection *default_ac = NULL;
	NMDevice *non_default_device = NULL;
	NMActiveConnection *non_default_ac = NULL;
	const GPtrArray *connections;
	int i;

	g_return_val_if_fail (NM_IS_APPLET (applet), NULL);
	g_return_val_if_fail (device != NULL, NULL);
	g_return_val_if_fail (*device == NULL, NULL);

	connections = nm_client_get_active_connections (applet->nm_client);
	for (i = 0; connections && (i < connections->len); i++) {
		NMActiveConnection *candidate = g_ptr_array_index (connections, i);
		NMDevice *candidate_dev;
		const GPtrArray *devices;

		devices = nm_active_connection_get_devices (candidate);
		if (!devices || !devices->len)
			continue;

		candidate_dev = g_ptr_array_index (devices, 0);

		if (   only_known_devices
		    && !get_device_class (candidate_dev, applet))
			continue;

		/* We have to return default connection/device even if they are of an
		 * unknown class - otherwise we may end up returning non
		 * default interface which has nothing to do with our default
		 * route, e.g. we may return an ethernet port when we have
		 * defult route going through bond */

		if (nm_active_connection_get_default (candidate)) {
			if (!default_ac) {
				*device = candidate_dev;
				default_ac = candidate;
			}
		} else {
			if (!non_default_ac) {
				non_default_device = candidate_dev;
				non_default_ac = candidate;
			}
		}
	}

	/* Prefer the default connection if one exists, otherwise return the first
	 * non-default connection.
	 */
	if (!default_ac && non_default_ac) {
		default_ac = non_default_ac;
		*device = non_default_device;
	}
	return default_ac;
}

GPtrArray *
applet_get_all_connections (NMApplet *applet)
{
	const GPtrArray *all_connections;
	GPtrArray *connections;
	int i;
	NMConnection *connection;
	NMSettingConnection *s_con;

	all_connections = nm_client_get_connections (applet->nm_client);
	connections = g_ptr_array_new_full (all_connections->len, g_object_unref);

	/* Ignore port connections unless they are wifi connections */
	for (i = 0; i < all_connections->len; i++) {
		connection = all_connections->pdata[i];

		s_con = nm_connection_get_setting_connection (connection);
		if (   s_con
		    && (   !nm_setting_connection_get_master (s_con)
		        || nm_connection_get_setting_wireless (connection)))
			g_ptr_array_add (connections, g_object_ref (connection));
	}

	return connections;
}

static NMActiveConnection *
applet_get_active_for_connection (NMApplet *applet, NMConnection *connection)
{
	const GPtrArray *active_list;
	int i;
	const char *cpath;

	cpath = nm_connection_get_path (connection);
	g_return_val_if_fail (cpath != NULL, NULL);

	active_list = nm_client_get_active_connections (applet->nm_client);
	for (i = 0; active_list && (i < active_list->len); i++) {
		NMActiveConnection *active = NM_ACTIVE_CONNECTION (g_ptr_array_index (active_list, i));
		NMRemoteConnection *conn = nm_active_connection_get_connection (active);

		if (conn) {
			const char *active_cpath = nm_connection_get_path (NM_CONNECTION (conn));

			if (active_cpath && !strcmp (active_cpath, cpath))
				return active;
		}
	}
	return NULL;
}

NMDevice *
applet_get_device_for_connection (NMApplet *applet, NMConnection *connection)
{
	const GPtrArray *active_list;
	const char *cpath;
	int i;

	cpath = nm_connection_get_path (connection);
	g_return_val_if_fail (cpath != NULL, NULL);

	active_list = nm_client_get_active_connections (applet->nm_client);
	for (i = 0; active_list && (i < active_list->len); i++) {
		NMActiveConnection *active = NM_ACTIVE_CONNECTION (g_ptr_array_index (active_list, i));
		NMRemoteConnection *ac_conn = nm_active_connection_get_connection (active);

		if (!g_strcmp0 (nm_connection_get_path (NM_CONNECTION (ac_conn)), cpath))
			return g_ptr_array_index (nm_active_connection_get_devices (active), 0);
	}
	return NULL;
}

typedef struct {
	NMApplet *applet;
	NMDevice *device;
	char *specific_object;
	NMConnection *connection;
} AppletItemActivateInfo;

static void
applet_item_activate_info_destroy (AppletItemActivateInfo *info)
{
	g_return_if_fail (info != NULL);

	if (info->device)
		g_object_unref (info->device);
	g_free (info->specific_object);
	if (info->connection)
		g_object_unref (info->connection);
	memset (info, 0, sizeof (AppletItemActivateInfo));
	g_free (info);
}

static void
add_and_activate_cb (GObject *client,
                     GAsyncResult *result,
                     gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);
	GError *error = NULL;
	NMActiveConnection *active;

	active = nm_client_add_and_activate_connection_finish (NM_CLIENT (client), result, &error);
	g_clear_object (&active);

	if (error) {
		const char *text = _("Failed to add/activate connection");
		const char *err_text = error->message ? error->message : _("Unknown error");

		utils_show_error_dialog (_("Connection failure"), text, err_text, FALSE, NULL);
		g_error_free (error);
	}

	applet_schedule_update_icon (applet);
	applet_schedule_update_menu (applet);
}

static void
applet_menu_item_activate_helper_new_connection (NMConnection *connection,
                                                 gboolean auto_created,
                                                 gboolean canceled,
                                                 gpointer user_data)
{
	AppletItemActivateInfo *info = user_data;

	if (canceled) {
		applet_item_activate_info_destroy (info);
		return;
	}

	g_return_if_fail (connection != NULL);

	/* Ask NM to add the new connection and activate it; NM will fill in the
	 * missing details based on the specific object and the device.
	 */
	nm_client_add_and_activate_connection_async (info->applet->nm_client,
	                                             connection,
	                                             info->device,
	                                             info->specific_object,
	                                             NULL,
	                                             add_and_activate_cb,
	                                             info->applet);

	applet_item_activate_info_destroy (info);
}

static void
disconnect_cb (GObject *device,
               GAsyncResult *result,
               gpointer user_data)

{
	NMApplet *applet = NM_APPLET (user_data);
	GError *error = NULL;

	nm_device_disconnect_finish (NM_DEVICE (device), result, &error);
	if (error) {
		const char *text = _("Device disconnect failed");
		const char *err_text = error->message ? error->message : _("Unknown error");

		utils_show_error_dialog (_("Disconnect failure"), text, err_text, FALSE, NULL);
		g_error_free (error);
	}

	applet_schedule_update_icon (applet);
	applet_schedule_update_menu (applet);
}

void
applet_menu_item_disconnect_helper (NMDevice *device,
                                    NMApplet *applet)
{
	g_return_if_fail (NM_IS_DEVICE (device));

	nm_device_disconnect_async (device, NULL, disconnect_cb, applet);
}

static void
activate_connection_cb (GObject *client,
                        GAsyncResult *result,
                        gpointer user_data)
{
	GError *error = NULL;
	NMActiveConnection *active;

	active = nm_client_activate_connection_finish (NM_CLIENT (client), result, &error);
	g_clear_object (&active);

	if (error) {
		const char *text = _("Connection activation failed");
		const char *err_text = error->message ? error->message : _("Unknown error");

		utils_show_error_dialog (_("Connection failure"), text, err_text, FALSE, NULL);
		g_error_free (error);
	}

	applet_schedule_update_icon (NM_APPLET (user_data));
}

void
applet_menu_item_activate_helper (NMDevice *device,
                                  NMConnection *connection,
                                  const char *specific_object,
                                  NMApplet *applet,
                                  gpointer dclass_data)
{
	AppletItemActivateInfo *info;
	NMADeviceClass *dclass;

	if (connection) {
		/* If the menu item had an associated connection already, just tell
		 * NM to activate that connection.
		 */
		nm_client_activate_connection_async (applet->nm_client,
		                                     connection,
		                                     device,
		                                     specific_object,
		                                     NULL,
		                                     activate_connection_cb,
		                                     applet);
		return;
	}

	g_return_if_fail (NM_IS_DEVICE (device));

	/* If no connection was given, ask the device class to create a new
	 * default connection for this device type.  This could be a wizard,
	 * and thus take a while.
	 */

	info = g_malloc0 (sizeof (AppletItemActivateInfo));
	info->applet = applet;
	info->specific_object = g_strdup (specific_object);
	info->device = g_object_ref (device);

	dclass = get_device_class (device, applet);
	g_assert (dclass);
	if (!dclass->new_auto_connection (device, dclass_data,
	                                  applet_menu_item_activate_helper_new_connection,
	                                  info))
		applet_item_activate_info_destroy (info);
}

void
applet_menu_item_add_complex_separator_helper (GtkWidget *menu,
                                               NMApplet *applet,
                                               const gchar *label)
{
	GtkWidget *menu_item, *box, *xlabel, *separator;

	if (INDICATOR_ENABLED (applet)) {
		/* Indicator doesn't draw complex separators */
		return;
	}

	menu_item = gtk_menu_item_new ();
	box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

	if (label) {
		xlabel = gtk_label_new (NULL);
		gtk_label_set_markup (GTK_LABEL (xlabel), label);

		separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
		g_object_set (G_OBJECT (separator), "valign", GTK_ALIGN_CENTER, NULL);
		gtk_box_pack_start (GTK_BOX (box), separator, TRUE, TRUE, 0);

		gtk_box_pack_start (GTK_BOX (box), xlabel, FALSE, FALSE, 2);
	}

	separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
	g_object_set (G_OBJECT (separator), "valign", GTK_ALIGN_CENTER, NULL);
	gtk_box_pack_start (GTK_BOX (box), separator, TRUE, TRUE, 0);

	g_object_set (G_OBJECT (menu_item),
		          "child", box,
		          "sensitive", FALSE,
		          NULL);

	gtk_menu_shell_append (GTK_MENU_SHELL (menu), menu_item);
}

GtkWidget *
applet_new_menu_item_helper (NMConnection *connection,
                             NMConnection *active,
                             gboolean add_active)
{
	GtkWidget *item = gtk_menu_item_new_with_label ("");

	if (add_active && (active == connection)) {
		char *markup;
		GtkWidget *label;

		/* Pure evil */
		label = gtk_bin_get_child (GTK_BIN (item));
		gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
		markup = g_markup_printf_escaped ("<b>%s</b>", nm_connection_get_id (connection));
		gtk_label_set_markup (GTK_LABEL (label), markup);
		g_free (markup);
	} else
		gtk_menu_item_set_label (GTK_MENU_ITEM (item), nm_connection_get_id (connection));

	return item;
}

#define TITLE_TEXT_R ((double) 0x5e / 255.0 )
#define TITLE_TEXT_G ((double) 0x5e / 255.0 )
#define TITLE_TEXT_B ((double) 0x5e / 255.0 )

static void
menu_item_draw_generic (GtkWidget *widget, cairo_t *cr)
{
	GtkWidget *label;
	PangoFontDescription *desc;
	PangoLayout *layout;
	GtkStyleContext *style;
	int width = 0, height = 0, owidth, oheight;
	gdouble extraheight = 0, extrawidth = 0;
	const char *text;
	gdouble xpadding = 10.0;
	gdouble ypadding = 5.0;
	gdouble postpadding = 0.0;

	label = gtk_bin_get_child (GTK_BIN (widget));
	text = gtk_label_get_text (GTK_LABEL (label));

	layout = gtk_widget_create_pango_layout (widget, NULL);
	style = gtk_widget_get_style_context (widget);
	gtk_style_context_get (style, gtk_style_context_get_state (style),
	                       "font", &desc,
	                       NULL);
	pango_font_description_set_variant (desc, PANGO_VARIANT_SMALL_CAPS);
	pango_font_description_set_weight (desc, PANGO_WEIGHT_SEMIBOLD);
	pango_layout_set_font_description (layout, desc);
	pango_layout_set_text (layout, text, -1);
	pango_cairo_update_layout (cr, layout);
	pango_layout_get_size (layout, &owidth, &oheight);
	width = owidth / PANGO_SCALE;
	height += oheight / PANGO_SCALE;

	cairo_save (cr);

	cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.0);
	cairo_rectangle (cr, 0, 0,
	                 (double) (width + 2 * xpadding),
	                 (double) (height + ypadding + postpadding));
	cairo_fill (cr);

	/* now the in-padding content */
	cairo_translate (cr, xpadding , ypadding);
	cairo_set_source_rgb (cr, TITLE_TEXT_R, TITLE_TEXT_G, TITLE_TEXT_B);
	cairo_move_to (cr, extrawidth, extraheight);
	pango_cairo_show_layout (cr, layout);

	cairo_restore(cr);

	pango_font_description_free (desc);
	g_object_unref (layout);

	gtk_widget_set_size_request (widget, width + 2 * xpadding, height + ypadding + postpadding);
}

static gboolean
menu_title_item_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
	menu_item_draw_generic (widget, cr);
	return TRUE;
}

GtkWidget *
applet_menu_item_create_device_item_helper (NMDevice *device,
                                            NMApplet *applet,
                                            const gchar *text)
{
	GtkWidget *item;

	item = gtk_menu_item_new_with_label (text);
	gtk_widget_set_sensitive (item, FALSE);
	if (!INDICATOR_ENABLED (applet))
		g_signal_connect (item, "draw", G_CALLBACK (menu_title_item_draw), NULL);
	return item;
}

static const char *
vpn_reasons_prefs[] = {
	PREF_DISABLE_REASON_DEVICE_DISCONNECTED,
	PREF_DISABLE_REASON_SERVICE_STOPPED,
	PREF_DISABLE_REASON_IP_CONFIG_INVALID,
	PREF_DISABLE_REASON_CONNECT_TIMEOUT,
	PREF_DISABLE_REASON_SERVICE_START_TIMEOUT,
	PREF_DISABLE_REASON_SERVICE_START_FAILED,
	PREF_DISABLE_REASON_NO_SECRETS,
	PREF_DISABLE_REASON_LOGIN_FAILED,
	PREF_DISABLE_REASON_USER_DISCONNECTED
};

static NMActiveConnectionStateReason 
vpn_reasons[] = {
    NM_ACTIVE_CONNECTION_STATE_REASON_DEVICE_DISCONNECTED,
    NM_ACTIVE_CONNECTION_STATE_REASON_SERVICE_STOPPED,
    NM_ACTIVE_CONNECTION_STATE_REASON_IP_CONFIG_INVALID,
    NM_ACTIVE_CONNECTION_STATE_REASON_CONNECT_TIMEOUT,
    NM_ACTIVE_CONNECTION_STATE_REASON_SERVICE_START_TIMEOUT,
    NM_ACTIVE_CONNECTION_STATE_REASON_SERVICE_START_FAILED,
    NM_ACTIVE_CONNECTION_STATE_REASON_NO_SECRETS,
    NM_ACTIVE_CONNECTION_STATE_REASON_LOGIN_FAILED,
	NM_ACTIVE_CONNECTION_STATE_REASON_USER_DISCONNECTED,
};

typedef enum {
    CONNECTED_NOTIFICATIONS    = 0,
    DISCONNECTED_NOTIFICATIONS = 1,
    VPN_NOTIFICATIONS          = 2,
} BaseNotificationTypes;

static gboolean 
is_vpn_notification_pref(const char *pref) 
{
	g_return_val_if_fail(pref != NULL, FALSE);

	for (int i = 0; i < G_N_ELEMENTS (vpn_reasons_prefs); i++) {
		if(!strcmp(pref, vpn_reasons_prefs[i])) {
			return TRUE;
		}
	}

    return FALSE;
}

void
applet_do_notify (NMApplet *applet,
                  const char *title,
                  const char *body,
                  const char *icon_name,
                  const char *pref)
{
	gs_unref_object GNotification *notify = NULL;
	GIcon *icon;
	char *escaped;

	g_return_if_fail (applet != NULL);
	g_return_if_fail (title != NULL);
	g_return_if_fail (body != NULL);

	if(pref && is_vpn_notification_pref(pref)) {
		if (g_settings_get_boolean (applet->gsettings, PREF_DISABLE_VPN_NOTIFICATIONS)) {
			return;
		}
	}

	if (pref && g_settings_get_boolean (applet->gsettings, pref))
		return;

	if (INDICATOR_ENABLED (applet)) {
#ifdef WITH_APPINDICATOR
		if (app_indicator_get_status (applet->app_indicator) == APP_INDICATOR_STATUS_PASSIVE)
			return;
#endif  /* WITH_APPINDICATOR */
	} else {
		if (!gtk_status_icon_is_embedded (applet->status_icon))
			return;
	}

	/* if we're not acting as a secret agent, don't notify either */
	if (!applet->agent)
		return;

	notify = g_notification_new (title);

	escaped = utils_escape_notify_body (body);
	g_notification_set_body (notify, escaped);
	g_free (escaped);

	icon = g_themed_icon_new (icon_name ?: "network-workgroup");
	g_notification_set_icon (notify, icon);
	g_object_unref (icon);

	if (pref) {
		g_notification_add_button_with_target (notify,
						       _("Don’t show this message again"),
						       "app.enable-pref", "s", pref);

	}

	g_application_send_notification (G_APPLICATION (applet), "nm-applet", notify);
}

static gboolean
animation_timeout (gpointer data)
{
	applet_schedule_update_icon (NM_APPLET (data));
	return TRUE;
}

static void
start_animation_timeout (NMApplet *applet)
{
	if (applet->animation_id == 0) {
		applet->animation_step = 0;
		applet->animation_id = g_timeout_add (100, animation_timeout, applet);
	}
}

static void
clear_animation_timeout (NMApplet *applet)
{
	if (applet->animation_id) {
		g_source_remove (applet->animation_id);
		applet->animation_id = 0;
		applet->animation_step = 0;
	}
}

static gboolean
applet_is_any_device_activating (NMApplet *applet)
{
	const GPtrArray *devices;
	int i;

	/* Check for activating devices */
	devices = nm_client_get_devices (applet->nm_client);
	for (i = 0; devices && (i < devices->len); i++) {
		NMDevice *candidate = NM_DEVICE (g_ptr_array_index (devices, i));
		NMDeviceState state;

		state = nm_device_get_state (candidate);
		if (state > NM_DEVICE_STATE_DISCONNECTED && state < NM_DEVICE_STATE_ACTIVATED)
			return TRUE;
	}
	return FALSE;
}

static gboolean
connection_is_vpn (NMConnection *connection)
{
	return    nm_connection_is_type (connection, NM_SETTING_VPN_SETTING_NAME)
	       || nm_connection_is_type (connection, NM_SETTING_WIREGUARD_SETTING_NAME);
}

static gboolean
applet_is_any_vpn_activating (NMApplet *applet)
{
	const GPtrArray *connections;
	int i;

	connections = nm_client_get_active_connections (applet->nm_client);
	for (i = 0; connections && (i < connections->len); i++) {
		NMActiveConnection *candidate = NM_ACTIVE_CONNECTION (g_ptr_array_index (connections, i));
		NMActiveConnectionState state;

		if (NM_IS_VPN_CONNECTION (candidate)) {
			state = nm_active_connection_get_state (candidate);
			if (state == NM_ACTIVE_CONNECTION_STATE_ACTIVATING)
				return TRUE;
		}
	}
	return FALSE;
}

static char *
make_active_failure_message (NMActiveConnection *active,
                             NMActiveConnectionStateReason reason,
                             NMApplet *applet,
							 char **pref)
{
	NMConnection *connection;
	const GPtrArray *devices;
	NMDevice *device;
	const char *id;

	g_return_val_if_fail (active != NULL, NULL);

	connection = (NMConnection *) nm_active_connection_get_connection (active);
	id = nm_connection_get_id (connection);

	switch (reason) {
	case NM_ACTIVE_CONNECTION_STATE_REASON_DEVICE_DISCONNECTED:
		devices = nm_active_connection_get_devices (active);
		device = devices && devices->len > 0 ? devices->pdata[0] : NULL;
		*pref = PREF_DISABLE_REASON_DEVICE_DISCONNECTED;
		if (device && nm_device_get_state (device) == NM_DEVICE_STATE_DISCONNECTED)
			return g_strdup_printf (_("\nThe VPN connection “%s” disconnected because the network connection was interrupted."), id);
		else
			return g_strdup_printf (_("\nThe VPN connection “%s” failed because the network connection was interrupted."), id);
	case NM_ACTIVE_CONNECTION_STATE_REASON_SERVICE_STOPPED:
		*pref = PREF_DISABLE_REASON_SERVICE_STOPPED;
		return g_strdup_printf (_("\nThe VPN connection “%s” failed because the VPN service stopped unexpectedly."), id);
	case NM_ACTIVE_CONNECTION_STATE_REASON_IP_CONFIG_INVALID:
		*pref = PREF_DISABLE_REASON_IP_CONFIG_INVALID;
		return g_strdup_printf (_("\nThe VPN connection “%s” failed because the VPN service returned invalid configuration."), id);
	case NM_ACTIVE_CONNECTION_STATE_REASON_CONNECT_TIMEOUT:
		*pref = PREF_DISABLE_REASON_CONNECT_TIMEOUT;
		return g_strdup_printf (_("\nThe VPN connection “%s” failed because the connection attempt timed out."), id);
	case NM_ACTIVE_CONNECTION_STATE_REASON_SERVICE_START_TIMEOUT:
		*pref = PREF_DISABLE_REASON_SERVICE_START_TIMEOUT;
		return g_strdup_printf (_("\nThe VPN connection “%s” failed because the VPN service did not start in time."), id);
	case NM_ACTIVE_CONNECTION_STATE_REASON_SERVICE_START_FAILED:
		*pref = PREF_DISABLE_REASON_SERVICE_START_FAILED;
		return g_strdup_printf (_("\nThe VPN connection “%s” failed because the VPN service failed to start."), id);
	case NM_ACTIVE_CONNECTION_STATE_REASON_NO_SECRETS:
		*pref = PREF_DISABLE_REASON_NO_SECRETS;
		return g_strdup_printf (_("\nThe VPN connection “%s” failed because there were no valid VPN secrets."), id);
	case NM_ACTIVE_CONNECTION_STATE_REASON_LOGIN_FAILED:
		*pref = PREF_DISABLE_REASON_LOGIN_FAILED;
		return g_strdup_printf (_("\nThe VPN connection “%s” failed because of invalid VPN secrets."), id);
	case NM_ACTIVE_CONNECTION_STATE_REASON_USER_DISCONNECTED:
		*pref = PREF_DISABLE_REASON_USER_DISCONNECTED;
		return g_strdup_printf (_("\nThe VPN connection “%s” disconnected because user interrupted."), id);
	default:
		break;
	}

	return g_strdup_printf (_("\nThe VPN connection “%s” failed."), id);
}

static void
vpn_active_connection_state_changed (NMVpnConnection *vpn,
                                     NMActiveConnectionState state,
                                     NMActiveConnectionStateReason reason,
                                     gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);
	const char *banner;
	char *title = NULL, *msg, *pref;
	gboolean device_activating, vpn_activating;

	device_activating = applet_is_any_device_activating (applet);
	vpn_activating = applet_is_any_vpn_activating (applet);

	pref = PREF_DISABLE_VPN_NOTIFICATIONS;

	switch (state) {
	case NM_ACTIVE_CONNECTION_STATE_ACTIVATING:
		/* Be sure to turn animation timeout on here since the dbus signals
		 * for new active connections might not have come through yet.
		 */
		vpn_activating = TRUE;
		break;
	case NM_ACTIVE_CONNECTION_STATE_ACTIVATED:
		banner = nm_vpn_connection_get_banner (vpn);
		if (banner && strlen (banner))
			msg = g_strdup_printf (_("VPN connection has been successfully established.\n\n%s\n"), banner);
		else
			msg = g_strdup (_("VPN connection has been successfully established.\n"));

		title = _("VPN Login Message");
		applet_do_notify (applet, title, msg, "gnome-lockscreen", pref);
		g_free (msg);
		break;
	case NM_ACTIVE_CONNECTION_STATE_DEACTIVATED:
		title = _("VPN Connection Failed");
		msg = make_active_failure_message (NM_ACTIVE_CONNECTION (vpn), reason, applet, &pref);
		applet_do_notify (applet, title, msg, "gnome-lockscreen", pref);
		g_free (msg);
		break;
	default:
		break;
	}

	if (device_activating || vpn_activating)
		start_animation_timeout (applet);
	else
		clear_animation_timeout (applet);

	applet_schedule_update_icon (applet);
	applet_schedule_update_menu (applet);
}

typedef struct {
	NMApplet *applet;
	char *vpn_name;
} VPNActivateInfo;

static void
activate_vpn_cb (GObject *client,
                 GAsyncResult *result,
                 gpointer user_data)
{
	VPNActivateInfo *info = (VPNActivateInfo *) user_data;
	NMActiveConnection *active;
	char *title, *msg, *name, *pref;
	GError *error = NULL;

	active = nm_client_activate_connection_finish (NM_CLIENT (client), result, &error);
	g_clear_object (&active);

	pref = PREF_DISABLE_VPN_NOTIFICATIONS;

	if (error) {
		clear_animation_timeout (info->applet);

		title = _("VPN Connection Failed");

		name = g_dbus_error_get_remote_error (error);
		if (name && strstr (name, "ServiceStartFailed")) {
			msg = g_strdup_printf (_("\nThe VPN connection “%s” failed because the VPN service failed to start.\n\n%s"),
			                       info->vpn_name, error->message);
			pref = PREF_DISABLE_REASON_SERVICE_START_FAILED;
		} else {
			msg = g_strdup_printf (_("\nThe VPN connection “%s” failed to start.\n\n%s"),
			                       info->vpn_name, error->message);
		}

		applet_do_notify (info->applet, title, msg, "gnome-lockscreen", pref);
		g_warning ("VPN Connection activation failed: (%s) %s", name, error->message);
		g_free (msg);
		g_free (name);
		g_error_free (error);
	}

	applet_schedule_update_icon (info->applet);
	applet_schedule_update_menu (info->applet);
	g_free (info->vpn_name);
	g_free (info);
}

static void
nma_menu_vpn_item_clicked (GtkMenuItem *item, gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);
	VPNActivateInfo *info;
	NMConnection *connection;
	NMActiveConnection *active;
	NMDevice *device = NULL;

	connection = NM_CONNECTION (g_object_get_data (G_OBJECT (item), "connection"));
	if (!connection) {
		g_warning ("%s: no connection associated with menu item!", __func__);
		return;
	}

	active = applet_get_active_for_connection (applet, connection);
	if (active) {
		/* Connection already active; disconnect it */
		nm_client_deactivate_connection (applet->nm_client, active, NULL, NULL);
		return;
	}

	if (nm_connection_is_type (connection, NM_SETTING_VPN_SETTING_NAME)) {
		active = applet_get_default_active_connection (applet, &device, FALSE);
		if (!active || !device) {
			/* FIXME: show a UI notification ? */
			g_warning ("%s: no active connection or device.", __func__);
			return;
		}
	}

	info = g_malloc0 (sizeof (VPNActivateInfo));
	info->applet = applet;
	info->vpn_name = g_strdup (nm_connection_get_id (connection));

	/* Connection inactive, activate */
	nm_client_activate_connection_async (applet->nm_client,
	                                     connection,
	                                     device,
	                                     active ? nm_object_get_path (NM_OBJECT (active)) : NULL,
	                                     NULL,
	                                     activate_vpn_cb,
	                                     info);
	start_animation_timeout (applet);
}

/*
 * nma_menu_configure_vpn_item_activate
 *
 * Signal function called when user clicks "Configure VPN..."
 *
 */
static void
nma_menu_configure_vpn_item_activate (GtkMenuItem *item, gpointer user_data)
{
	const char *argv[] = { BINDIR "/nm-connection-editor", "--show", "--type", NM_SETTING_VPN_SETTING_NAME, NULL};

	g_spawn_async (NULL, (gchar **) argv, NULL, 0, NULL, NULL, NULL, NULL);
}

/*
 * nma_menu_add_vpn_item_activate
 *
 * Signal function called when user clicks "Add a VPN connection..."
 *
 */
static void
nma_menu_add_vpn_item_activate (GtkMenuItem *item, gpointer user_data)
{
	const char *argv[] = { BINDIR "/nm-connection-editor", "--create", "--type", NM_SETTING_VPN_SETTING_NAME, NULL};

	g_spawn_async (NULL, (gchar **) argv, NULL, 0, NULL, NULL, NULL, NULL);
}

static NMVpnConnectionState
ac_state_to_vpn_state (NMActiveConnectionState ac_state)
{
	switch (ac_state) {
	case NM_ACTIVE_CONNECTION_STATE_UNKNOWN:
		return NM_VPN_CONNECTION_STATE_UNKNOWN;
	case NM_ACTIVE_CONNECTION_STATE_ACTIVATING:
		return NM_VPN_CONNECTION_STATE_PREPARE;
	case NM_ACTIVE_CONNECTION_STATE_ACTIVATED:
	case NM_ACTIVE_CONNECTION_STATE_DEACTIVATING:
		return NM_VPN_CONNECTION_STATE_ACTIVATED;
	case NM_ACTIVE_CONNECTION_STATE_DEACTIVATED:
		return NM_VPN_CONNECTION_STATE_DISCONNECTED;
	}

	nm_assert_not_reached ();
	return NM_VPN_CONNECTION_STATE_UNKNOWN;
}

/*
 * applet_get_active_vpn_connection:
 *
 * Gets a VPN connection along with its state. If there are more, ones that
 * are not yet fully activated are preferred.
 *
 */
static NMActiveConnection *
applet_get_active_vpn_connection (NMApplet *applet,
                                  NMVpnConnectionState *out_state)
{
	const GPtrArray *active_list;
	NMActiveConnection *ret = NULL;
	NMVpnConnectionState state = NM_VPN_CONNECTION_STATE_UNKNOWN;
	int i;

	active_list = nm_client_get_active_connections (applet->nm_client);
	for (i = 0; active_list && (i < active_list->len); i++) {
		NMActiveConnection *candidate;
		NMConnection *connection;

		candidate = g_ptr_array_index (active_list, i);

		connection = (NMConnection *) nm_active_connection_get_connection (candidate);
		if (!connection)
			continue;

		if (!connection_is_vpn (connection))
			continue;

		ret = candidate;
		if (nm_connection_is_type (connection, NM_SETTING_VPN_SETTING_NAME)) {
			state = nm_vpn_connection_get_vpn_state (NM_VPN_CONNECTION (candidate));
		} else {
			NMActiveConnectionState ac_state;

			ac_state = nm_active_connection_get_state (candidate);
			state = ac_state_to_vpn_state (ac_state);
		}
		if (state != NM_VPN_CONNECTION_STATE_ACTIVATED)
			break;
	}

	if (ret && out_state)
		*out_state = state;

	return ret;
}

/*
 * nma_menu_add_separator_item
 *
 */
static void
nma_menu_add_separator_item (GtkWidget *menu)
{
	GtkWidget *menu_item;

	menu_item = gtk_separator_menu_item_new ();
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), menu_item);
	gtk_widget_show (menu_item);
}


/*
 * nma_menu_add_text_item
 *
 * Add a non-clickable text item to a menu
 *
 */
static void nma_menu_add_text_item (GtkWidget *menu, char *text)
{
	GtkWidget		*menu_item;

	g_return_if_fail (text != NULL);
	g_return_if_fail (menu != NULL);

	menu_item = gtk_menu_item_new_with_label (text);
	gtk_widget_set_sensitive (menu_item, FALSE);

	gtk_menu_shell_append (GTK_MENU_SHELL (menu), menu_item);
	gtk_widget_show (menu_item);
}

static gint
sort_devices_by_description (gconstpointer a, gconstpointer b)
{
	NMDevice *aa = NM_DEVICE (a);
	NMDevice *bb = NM_DEVICE (b);
	const char *aa_desc;
	const char *bb_desc;

	aa_desc = nm_device_get_description (aa);
	bb_desc = nm_device_get_description (bb);

	return g_strcmp0 (aa_desc, bb_desc);
}

static gboolean
nm_g_ptr_array_contains (const GPtrArray *haystack, gpointer needle)
{
	int i;

	for (i = 0; haystack && (i < haystack->len); i++) {
		if (g_ptr_array_index (haystack, i) == needle)
			return TRUE;
	}
	return FALSE;
}

static NMConnection *
applet_find_active_connection_for_device (NMDevice *device,
                                          NMApplet *applet,
                                          NMActiveConnection **out_active)
{
	const GPtrArray *active_connections;
	int i;

	g_return_val_if_fail (NM_IS_DEVICE (device), NULL);
	g_return_val_if_fail (NM_IS_APPLET (applet), NULL);
	if (out_active)
		g_return_val_if_fail (*out_active == NULL, NULL);

	active_connections = nm_client_get_active_connections (applet->nm_client);
	for (i = 0; i < active_connections->len; i++) {
		NMRemoteConnection *conn;
		NMActiveConnection *active;
		const GPtrArray *devices;

		active = NM_ACTIVE_CONNECTION (g_ptr_array_index (active_connections, i));
		devices = nm_active_connection_get_devices (active);
		conn = nm_active_connection_get_connection (active);

		/* Skip VPN connections */
		if (nm_active_connection_get_vpn (active))
			continue;

		if (!devices || !conn)
			continue;

		if (!nm_g_ptr_array_contains (devices, device))
			continue;

		if (out_active)
			*out_active = active;
		return NM_CONNECTION (conn);
	}

	return NULL;
}

gboolean
nma_menu_device_check_unusable (NMDevice *device)
{
	switch (nm_device_get_state (device)) {
	case NM_DEVICE_STATE_UNKNOWN:
	case NM_DEVICE_STATE_UNAVAILABLE:
	case NM_DEVICE_STATE_UNMANAGED:
		return TRUE;
	default:
		break;
	}
	return FALSE;
}


struct AppletDeviceMenuInfo {
	NMDevice *device;
	NMApplet *applet;
};

static void
applet_device_info_destroy (gpointer data, GClosure *closure)
{
	struct AppletDeviceMenuInfo *info = data;

	g_return_if_fail (info != NULL);

	if (info->device)
		g_object_unref (info->device);
	memset (info, 0, sizeof (struct AppletDeviceMenuInfo));
	g_free (info);
}

static void
applet_device_disconnect_db (GtkMenuItem *item, gpointer user_data)
{
	struct AppletDeviceMenuInfo *info = user_data;

	applet_menu_item_disconnect_helper (info->device,
	                                    info->applet);
}

GtkWidget *
nma_menu_device_get_menu_item (NMDevice *device,
                               NMApplet *applet,
                               const char *unavailable_msg)
{
	GtkWidget *item = NULL;
	gboolean managed = TRUE;

	if (!unavailable_msg) {
		if (nm_device_get_firmware_missing (device))
			unavailable_msg = _("device not ready (firmware missing)");
		else
			unavailable_msg = _("device not ready");
	}

	switch (nm_device_get_state (device)) {
	case NM_DEVICE_STATE_UNKNOWN:
	case NM_DEVICE_STATE_UNAVAILABLE:
		item = gtk_menu_item_new_with_label (unavailable_msg);
		gtk_widget_set_sensitive (item, FALSE);
		break;
	case NM_DEVICE_STATE_DISCONNECTED:
		unavailable_msg = _("disconnected");
		item = gtk_menu_item_new_with_label (unavailable_msg);
		gtk_widget_set_sensitive (item, FALSE);
		break;
	case NM_DEVICE_STATE_UNMANAGED:
		managed = FALSE;
		break;
	case NM_DEVICE_STATE_PREPARE:
	case NM_DEVICE_STATE_CONFIG:
	case NM_DEVICE_STATE_NEED_AUTH:
	case NM_DEVICE_STATE_IP_CONFIG:
	case NM_DEVICE_STATE_ACTIVATED:
	{
		struct AppletDeviceMenuInfo *info = g_new0 (struct AppletDeviceMenuInfo, 1);
		info->device = g_object_ref (device);
		info->applet = applet;
		item = gtk_menu_item_new_with_label (_("Disconnect"));
		g_signal_connect_data (item, "activate",
		                       G_CALLBACK (applet_device_disconnect_db),
		                       info,
		                       applet_device_info_destroy, 0);
		gtk_widget_set_sensitive (item, TRUE);
		break;
	}
	default:
		managed = nm_device_get_managed (device);
		break;
	}

	if (!managed) {
		item = gtk_menu_item_new_with_label (_("device not managed"));
		gtk_widget_set_sensitive (item, FALSE);
	}

	return item;
}

static int
add_device_items (NMDeviceType type, const GPtrArray *all_devices,
                  const GPtrArray *all_connections,
                  GtkWidget *menu, NMApplet *applet)
{
	GSList *devices = NULL, *iter;
	int i, n_devices = 0;

	for (i = 0; all_devices && (i < all_devices->len); i++) {
		NMDevice *device = all_devices->pdata[i];

		if (nm_device_get_device_type (device) == type) {
			n_devices++;
			devices = g_slist_prepend (devices, device);
		}
	}
	devices = g_slist_sort (devices, sort_devices_by_description);

	for (iter = devices; iter; iter = iter->next) {
		NMDevice *device = iter->data;
		NMADeviceClass *dclass;
		NMConnection *active;
		GPtrArray *connections;
		gboolean added;

		dclass = get_device_class (device, applet);
		if (!dclass)
			continue;

		connections = nm_device_filter_connections (device, all_connections);
		active = applet_find_active_connection_for_device (device, applet, NULL);

		added = dclass->add_menu_item (device, n_devices > 1, connections, active, menu, applet);

		g_ptr_array_unref (connections);

		if (INDICATOR_ENABLED (applet) && added)
			gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
	}

	g_slist_free (devices);
	return n_devices;
}

static void
nma_menu_add_devices (GtkWidget *menu, NMApplet *applet)
{
	const GPtrArray *all_devices;
	GPtrArray *all_connections;
	gint n_items;

	all_connections = applet_get_all_connections (applet);
	all_devices = nm_client_get_devices (applet->nm_client);

	n_items = 0;
	n_items += add_device_items  (NM_DEVICE_TYPE_ETHERNET,
	                              all_devices, all_connections, menu, applet);
	n_items += add_device_items  (NM_DEVICE_TYPE_WIFI,
	                              all_devices, all_connections, menu, applet);
	n_items += add_device_items  (NM_DEVICE_TYPE_MODEM,
	                              all_devices, all_connections, menu, applet);
	n_items += add_device_items  (NM_DEVICE_TYPE_BT,
	                              all_devices, all_connections, menu, applet);

	g_ptr_array_unref (all_connections);

	if (!n_items)
		nma_menu_add_text_item (menu, _("No network devices available"));
}

static int
sort_vpn_connections (gconstpointer a, gconstpointer b)
{
	NMConnection **ca = (NMConnection **) a;
	NMConnection **cb = (NMConnection **) b;

	return strcmp (nm_connection_get_id (NM_CONNECTION (*ca)), nm_connection_get_id (NM_CONNECTION (*cb)));
}

static GPtrArray *
get_vpn_connections (NMApplet *applet)
{
	GPtrArray *all_connections, *vpn_connections;
	int i;

	all_connections = applet_get_all_connections (applet);
	vpn_connections = g_ptr_array_new_full (5, g_object_unref);

	for (i = 0; i < all_connections->len; i++) {
		NMConnection *connection = NM_CONNECTION (all_connections->pdata[i]);

		if (!connection_is_vpn (connection))
			continue;

		g_ptr_array_add (vpn_connections, g_object_ref (connection));
	}

	g_ptr_array_unref (all_connections);

	g_ptr_array_sort (vpn_connections, sort_vpn_connections);
	return vpn_connections;
}

static void
nma_menu_add_vpn_submenu (GtkWidget *menu, NMApplet *applet)
{
	GtkMenu *vpn_menu;
	GtkMenuItem *item;
	GPtrArray *list;
	int i;

	vpn_menu = GTK_MENU (gtk_menu_new ());

	item = GTK_MENU_ITEM (gtk_menu_item_new_with_mnemonic (_("_VPN Connections")));
	gtk_menu_item_set_submenu (item, GTK_WIDGET (vpn_menu));
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), GTK_WIDGET (item));
	gtk_widget_show (GTK_WIDGET (item));

	list = get_vpn_connections (applet);
	for (i = 0; i < list->len; i++) {
		NMConnection *connection = NM_CONNECTION (list->pdata[i]);
		NMActiveConnection *active;
		const char *name;
		NMState state;

		name = nm_connection_get_id (connection);

		item = GTK_MENU_ITEM (gtk_check_menu_item_new_with_label (name));

		/* If no VPN connections are active, draw all menu items enabled. If
		 * >= 1 VPN connections are active, only the active VPN menu item is
		 * drawn enabled.
		 */
		active = applet_get_active_for_connection (applet, connection);

		state = nm_client_get_state (applet->nm_client);
		if (   state != NM_STATE_CONNECTED_LOCAL
		    && state != NM_STATE_CONNECTED_SITE
		    && state != NM_STATE_CONNECTED_GLOBAL)
			gtk_widget_set_sensitive (GTK_WIDGET (item), FALSE);
		else
			gtk_widget_set_sensitive (GTK_WIDGET (item), TRUE);

		gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item), !!active);

		g_object_set_data_full (G_OBJECT (item), "connection",
		                        g_object_ref (connection),
		                        (GDestroyNotify) g_object_unref);

		g_signal_connect (item, "activate", G_CALLBACK (nma_menu_vpn_item_clicked), applet);
		gtk_menu_shell_append (GTK_MENU_SHELL (vpn_menu), GTK_WIDGET (item));
		gtk_widget_show (GTK_WIDGET (item));
	}

	/* Draw a separator, but only if we have VPN connections above it */
	if (list->len) {
		nma_menu_add_separator_item (GTK_WIDGET (vpn_menu));
		item = GTK_MENU_ITEM (gtk_menu_item_new_with_mnemonic (_("_Configure VPN…")));
		g_signal_connect (item, "activate", G_CALLBACK (nma_menu_configure_vpn_item_activate), applet);
	} else {
		item = GTK_MENU_ITEM (gtk_menu_item_new_with_mnemonic (_("_Add a VPN connection…")));
		g_signal_connect (item, "activate", G_CALLBACK (nma_menu_add_vpn_item_activate), applet);
	}
	gtk_menu_shell_append (GTK_MENU_SHELL (vpn_menu), GTK_WIDGET (item));
	gtk_widget_show (GTK_WIDGET (item));

	g_ptr_array_unref (list);
}


static void
nma_set_wifi_enabled_cb (GtkWidget *widget, NMApplet *applet)
{
	gboolean state;

	g_return_if_fail (applet != NULL);

	state = gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (widget));
	nm_client_wireless_set_enabled (applet->nm_client, state);
}

static void
nma_set_wwan_enabled_cb (GtkWidget *widget, NMApplet *applet)
{
	gboolean state;

	g_return_if_fail (applet != NULL);

	state = gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (widget));
	nm_client_wwan_set_enabled (applet->nm_client, state);
}

static void
nma_set_networking_enabled_cb (GtkWidget *widget, NMApplet *applet)
{
	gboolean state;

	g_return_if_fail (applet != NULL);

	state = gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (widget));
	nm_client_networking_set_enabled (applet->nm_client, state, NULL);
}

static void 
set_notification_menu_item_state(GtkWidget *widget, gpointer data) 
{
	gboolean state;
	state = gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (data));
    if (GTK_IS_CHECK_MENU_ITEM(widget)) {
		gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (widget), !state);
    }
}

static void
change_vpn_notifications_state(NMApplet *applet, gboolean state)
{
	for (int i = 0; i < G_N_ELEMENTS (vpn_reasons_prefs); i++) {
		g_settings_set_boolean (applet->gsettings, vpn_reasons_prefs[i], state);
	}
}

static void
nma_set_notifications_enabled_cb (GtkWidget *widget, NMApplet *applet)
{
	gboolean state;

	g_return_if_fail (applet != NULL);

	state = gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (widget));

	g_settings_set_boolean (applet->gsettings,
	                        PREF_DISABLE_CONNECTED_NOTIFICATIONS,
	                        !state);
	g_settings_set_boolean (applet->gsettings,
	                        PREF_DISABLE_DISCONNECTED_NOTIFICATIONS,
	                        !state);
	g_settings_set_boolean (applet->gsettings,
	                        PREF_DISABLE_VPN_NOTIFICATIONS,
	                        !state);
	g_settings_set_boolean (applet->gsettings,
	                        PREF_SUPPRESS_WIFI_NETWORKS_AVAILABLE,
	                        !state);

	change_vpn_notifications_state(applet, !state);

	gtk_container_foreach(GTK_CONTAINER(applet->notifications_menu), set_notification_menu_item_state, widget);
}

static gboolean
has_usable_wifi (NMApplet *applet)
{
	const GPtrArray *devices;
	int i;

	if (!nm_client_wireless_get_enabled (applet->nm_client))
		return FALSE;

	devices = nm_client_get_devices (applet->nm_client);
	if (!devices)
		return FALSE;

	for (i = 0; i < devices->len; i++) {
		NMDevice *device = devices->pdata[i];

		if (   NM_IS_DEVICE_WIFI (device)
		    && (nm_device_get_state (device) >= NM_DEVICE_STATE_DISCONNECTED))
			return TRUE;
	}

	return FALSE;
}

/*
 * nma_menu_show_cb
 *
 * Pop up the wifi networks menu
 *
 */
static void nma_menu_show_cb (GtkWidget *menu, NMApplet *applet)
{
	g_return_if_fail (menu != NULL);
	g_return_if_fail (applet != NULL);

	if (applet->status_icon)
		gtk_status_icon_set_tooltip_text (applet->status_icon, NULL);

	if (!nm_client_get_nm_running (applet->nm_client)) {
		nma_menu_add_text_item (menu, _("NetworkManager is not running…"));
		return;
	}

	if (nm_client_get_state (applet->nm_client) == NM_STATE_ASLEEP) {
		nma_menu_add_text_item (menu, _("Networking disabled"));
		return;
	}

	nma_menu_add_devices (menu, applet);

	if (has_usable_wifi (applet)) {
		/* Add the "Hidden Wi-Fi network..." entry */
		nma_menu_add_hidden_network_item (menu, applet);
		nma_menu_add_create_network_item (menu, applet);
		nma_menu_add_separator_item (menu);
	}
	nma_menu_add_vpn_submenu (menu, applet);

	if (!INDICATOR_ENABLED (applet))
		gtk_widget_show_all (menu);
}

static gboolean
destroy_old_menu (gpointer user_data)
{
	g_object_unref (user_data);
	return FALSE;
}

static void
nma_menu_deactivate_cb (GtkWidget *widget, NMApplet *applet)
{
	/* Must punt the destroy to a low-priority idle to ensure that
	 * the menu items don't get destroyed before any 'activate' signal
	 * fires for an item.
	 */
	g_signal_handlers_disconnect_by_func (applet->menu, G_CALLBACK (nma_menu_deactivate_cb), applet);
	g_idle_add_full (G_PRIORITY_LOW, destroy_old_menu, applet->menu, NULL);
	applet->menu = NULL;

	applet_stop_wifi_scan (applet, NULL);

	/* Re-set the tooltip */
	gtk_status_icon_set_tooltip_text (applet->status_icon, applet->tip);
}

static gboolean
is_permission_yes (NMApplet *applet, NMClientPermission perm)
{
	if (   applet->permissions[perm] == NM_CLIENT_PERMISSION_RESULT_YES
	    || applet->permissions[perm] == NM_CLIENT_PERMISSION_RESULT_AUTH)
		return TRUE;
	return FALSE;
}

/*
 * nma_context_menu_update
 *
 */
static void
nma_context_menu_update (NMApplet *applet)
{
	NMState state;
	gboolean net_enabled = TRUE;
	gboolean have_wifi = FALSE;
	gboolean have_wwan = FALSE;
	gboolean wifi_hw_enabled;
	gboolean wwan_hw_enabled;
	gboolean notifications_enabled = TRUE;
	gboolean sensitive = FALSE;

	state = nm_client_get_state (applet->nm_client);
	sensitive = (   state == NM_STATE_CONNECTED_LOCAL
	             || state == NM_STATE_CONNECTED_SITE
	             || state == NM_STATE_CONNECTED_GLOBAL);
	gtk_widget_set_sensitive (applet->info_menu_item, sensitive);

	/* Update checkboxes, and block 'toggled' signal when updating so that the
	 * callback doesn't get triggered.
	 */

	/* Enabled Networking */
	g_signal_handler_block (G_OBJECT (applet->networking_enabled_item),
	                        applet->networking_enabled_toggled_id);
	net_enabled = nm_client_networking_get_enabled (applet->nm_client);
	gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (applet->networking_enabled_item),
	                                net_enabled && (state != NM_STATE_ASLEEP));
	g_signal_handler_unblock (G_OBJECT (applet->networking_enabled_item),
	                          applet->networking_enabled_toggled_id);
	gtk_widget_set_sensitive (applet->networking_enabled_item,
	                          is_permission_yes (applet, NM_CLIENT_PERMISSION_ENABLE_DISABLE_NETWORK));

	/* Enabled Wi-Fi */
	g_signal_handler_block (G_OBJECT (applet->wifi_enabled_item),
	                        applet->wifi_enabled_toggled_id);
	gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (applet->wifi_enabled_item),
	                                nm_client_wireless_get_enabled (applet->nm_client));
	g_signal_handler_unblock (G_OBJECT (applet->wifi_enabled_item),
	                          applet->wifi_enabled_toggled_id);

	wifi_hw_enabled = nm_client_wireless_hardware_get_enabled (applet->nm_client);
	gtk_widget_set_sensitive (GTK_WIDGET (applet->wifi_enabled_item),
	                          wifi_hw_enabled && is_permission_yes (applet, NM_CLIENT_PERMISSION_ENABLE_DISABLE_WIFI));

	/* Enabled Mobile Broadband */
	g_signal_handler_block (G_OBJECT (applet->wwan_enabled_item),
	                        applet->wwan_enabled_toggled_id);
	gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (applet->wwan_enabled_item),
	                                nm_client_wwan_get_enabled (applet->nm_client));
	g_signal_handler_unblock (G_OBJECT (applet->wwan_enabled_item),
	                          applet->wwan_enabled_toggled_id);

	wwan_hw_enabled = nm_client_wwan_hardware_get_enabled (applet->nm_client);
	gtk_widget_set_sensitive (GTK_WIDGET (applet->wwan_enabled_item),
	                          wwan_hw_enabled && is_permission_yes (applet, NM_CLIENT_PERMISSION_ENABLE_DISABLE_WWAN));

	if (!INDICATOR_ENABLED (applet)) {
		/* Enabled notifications */
		g_signal_handler_block (G_OBJECT (applet->notifications_enabled_item),
			                    applet->notifications_enabled_toggled_id);
		if (   g_settings_get_boolean (applet->gsettings, PREF_DISABLE_CONNECTED_NOTIFICATIONS)
			&& g_settings_get_boolean (applet->gsettings, PREF_DISABLE_DISCONNECTED_NOTIFICATIONS)
			&& g_settings_get_boolean (applet->gsettings, PREF_DISABLE_VPN_NOTIFICATIONS)
			&& g_settings_get_boolean (applet->gsettings, PREF_SUPPRESS_WIFI_NETWORKS_AVAILABLE))
			notifications_enabled = FALSE;
		gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (applet->notifications_enabled_item), notifications_enabled);
		g_signal_handler_unblock (G_OBJECT (applet->notifications_enabled_item),
			                      applet->notifications_enabled_toggled_id);
	}

	/* Don't show wifi-specific stuff if wifi is off */
	if (state != NM_STATE_ASLEEP) {
		const GPtrArray *devices;
		int i;

		devices = nm_client_get_devices (applet->nm_client);
		for (i = 0; devices && (i < devices->len); i++) {
			NMDevice *candidate = g_ptr_array_index (devices, i);

			if (NM_IS_DEVICE_WIFI (candidate))
				have_wifi = TRUE;
			else if (NM_IS_DEVICE_MODEM (candidate))
				have_wwan = TRUE;
		}
	}

	if (have_wifi)
		gtk_widget_show_all (applet->wifi_enabled_item);
	else
		gtk_widget_hide (applet->wifi_enabled_item);

	if (have_wwan)
		gtk_widget_show_all (applet->wwan_enabled_item);
	else
		gtk_widget_hide (applet->wwan_enabled_item);
}

static void
ce_child_setup (gpointer user_data G_GNUC_UNUSED)
{
	/* We are in the child process at this point */
	pid_t pid = getpid ();
	setpgid (pid, pid);
}

static void
nma_edit_connections_cb (void)
{
	char *argv[2];
	GError *error = NULL;
	gboolean success;

	argv[0] = BINDIR "/nm-connection-editor";
	argv[1] = NULL;

	success = g_spawn_async ("/", argv, NULL, 0, &ce_child_setup, NULL, NULL, &error);
	if (!success) {
		g_warning ("Error launching connection editor: %s", error->message);
		g_error_free (error);
	}
}

static void
applet_connection_info_cb (NMApplet *applet)
{
	applet_info_dialog_show (applet);
}

static void
nma_menu_notification_item_clicked (GtkMenuItem *item, NMApplet *applet)
{
	const char *pref = g_object_get_data(G_OBJECT(item), "notification-pref");
	gboolean state = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item));
	g_settings_set_boolean (applet->gsettings, pref, state);
}

static void 
update_notification_prefs(GtkWidget *widget, gpointer applet) 
{
    if (GTK_IS_CHECK_BUTTON(widget)) {
        gboolean is_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
		const char *pref = g_object_get_data(G_OBJECT(widget), "connection-reason-pref");
		g_settings_set_boolean (((NMApplet *)applet)->gsettings, pref, is_active);
    }
}

static void 
on_vpn_notifications_dialog_response(GtkDialog *dialog, gint response_id, NMApplet *applet) 
{
	if (response_id == GTK_RESPONSE_OK) {
		gtk_container_foreach(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), 
						  	  update_notification_prefs, applet);
	}

	gtk_widget_destroy(GTK_WIDGET(dialog));
}

static GtkWidget* 
create_check_button_for_reason(NMApplet *applet, NMActiveConnectionStateReason reason) 
{
	GtkWidget *check;
    const char *label;
    const char *pref;
	bool is_active;

    switch (reason) {
        case NM_ACTIVE_CONNECTION_STATE_REASON_DEVICE_DISCONNECTED:
            label = "Отключить уведомление о перерывании соединения";
            pref = PREF_DISABLE_REASON_DEVICE_DISCONNECTED;
            break;
        case NM_ACTIVE_CONNECTION_STATE_REASON_SERVICE_STOPPED:
            label = "Отключить уведомление о неожиданном завершении работы службы VPN";
            pref = PREF_DISABLE_REASON_SERVICE_STOPPED;
            break;
        case NM_ACTIVE_CONNECTION_STATE_REASON_IP_CONFIG_INVALID:
            label = "Отключить уведомление о недопустимой конфигурации службы VPN";
            pref = PREF_DISABLE_REASON_IP_CONFIG_INVALID;
            break;
        case NM_ACTIVE_CONNECTION_STATE_REASON_CONNECT_TIMEOUT:
            label = "Отключить уведомление о превышении времени ожидания";
            pref = PREF_DISABLE_REASON_CONNECT_TIMEOUT;
            break;
        case NM_ACTIVE_CONNECTION_STATE_REASON_SERVICE_START_TIMEOUT:
            label = "Отключить уведомление о том, что служба VPN не была запущена вовремя";
            pref = PREF_DISABLE_REASON_SERVICE_START_TIMEOUT;
            break;
        case NM_ACTIVE_CONNECTION_STATE_REASON_SERVICE_START_FAILED:
            label = "Отключить уведомление о сбое запуска службы VPN";
            pref = PREF_DISABLE_REASON_SERVICE_START_FAILED;
            break;
        case NM_ACTIVE_CONNECTION_STATE_REASON_NO_SECRETS:
            label = "Отключить уведомление об отсутствии действительного пароля";
            pref = PREF_DISABLE_REASON_NO_SECRETS;
            break;
        case NM_ACTIVE_CONNECTION_STATE_REASON_LOGIN_FAILED:
            label = "Отключить уведомление о недействительном пароле";
            pref = PREF_DISABLE_REASON_LOGIN_FAILED;
            break;
        case NM_ACTIVE_CONNECTION_STATE_REASON_USER_DISCONNECTED:
            label = "Отключить уведомление о ручном разрыве подключения";
            pref = PREF_DISABLE_REASON_USER_DISCONNECTED;
            break;
        default:
            return NULL;
    }

	check = gtk_check_button_new_with_label(label);
    is_active = g_settings_get_boolean(applet->gsettings, pref);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), is_active);
    g_object_set_data_full(G_OBJECT(check), "connection-reason-pref", GINT_TO_POINTER(pref), NULL);

    return check;
}

static void
nma_menu_configure_notify_item_activate (GtkMenuItem *item, NMApplet *applet)
{
	GtkWidget *dialog, *content_area;

	dialog = gtk_dialog_new_with_buttons(_("Manage VPN notifications"), NULL, 
										 GTK_DIALOG_MODAL, "Сохранить",  
										 GTK_RESPONSE_OK, NULL);

	g_signal_connect(dialog, "response", G_CALLBACK(on_vpn_notifications_dialog_response), applet);
	gtk_window_set_default_size(GTK_WINDOW(dialog), 500, 500);

	content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

	for(int i = 0; i < G_N_ELEMENTS (vpn_reasons); i++) 
	{
		GtkWidget *check = create_check_button_for_reason(applet, vpn_reasons[i]);
        if (check) {
            gtk_box_pack_start(GTK_BOX(content_area), check, TRUE, TRUE, 0);
        }
	}
	
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
}

static GtkMenuItem* 
create_notifications_menu_item(NMApplet *applet, BaseNotificationTypes type)
{
	GtkMenuItem *item;
    const char *label;
    const char *pref;
	bool is_active;
	
	switch (type) {
		case CONNECTED_NOTIFICATIONS:
			label = "Отключить уведомления о подключении";
			pref = PREF_DISABLE_CONNECTED_NOTIFICATIONS;
			break;
		case DISCONNECTED_NOTIFICATIONS:
			label = "Отключить уведомления об отключении";
			pref = PREF_DISABLE_DISCONNECTED_NOTIFICATIONS;
			break;
		case VPN_NOTIFICATIONS:
			label = "Отключить уведомления о статусе VPN";
			pref = PREF_DISABLE_VPN_NOTIFICATIONS;
			break;
		default:
			return NULL;
	}

	item = GTK_MENU_ITEM (gtk_check_menu_item_new_with_label (label));
	is_active = g_settings_get_boolean (applet->gsettings, pref);
	gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item), is_active);
	g_object_set_data_full(G_OBJECT(item), "notification-pref", GINT_TO_POINTER(pref), NULL);
	g_signal_connect (item, "activate", G_CALLBACK (nma_menu_notification_item_clicked), applet);

	return item;
}

static void
nma_menu_add_notification_submenu (GtkWidget *menu, NMApplet *applet)
{
	GtkMenu *notifications_menu;
	GtkMenuItem *item;

	notifications_menu = GTK_MENU (gtk_menu_new ());

	item = GTK_MENU_ITEM (gtk_menu_item_new_with_mnemonic (_("Notification settings")));
	gtk_menu_item_set_submenu (item, GTK_WIDGET (notifications_menu));
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), GTK_WIDGET (item));
	gtk_widget_show (GTK_WIDGET (item));

	for (BaseNotificationTypes i = CONNECTED_NOTIFICATIONS; i <= VPN_NOTIFICATIONS; i++) 
	{
		item = create_notifications_menu_item(applet, i);
		if(item) {
			gtk_menu_shell_append (GTK_MENU_SHELL (notifications_menu), GTK_WIDGET (item));
		}
	}

	nma_menu_add_separator_item (GTK_WIDGET (notifications_menu));
	item = GTK_MENU_ITEM (gtk_menu_item_new_with_mnemonic (_("Detailed VPN notification settings")));
	g_signal_connect (item, "activate", G_CALLBACK (nma_menu_configure_notify_item_activate), applet);
	gtk_menu_shell_append (GTK_MENU_SHELL (notifications_menu), GTK_WIDGET (item));
	gtk_widget_show (GTK_WIDGET (item));

	applet->notifications_menu = notifications_menu;
}

/*
 * nma_context_menu_populate
 *
 * Populate the contextual popup menu.
 *
 */
static void nma_context_menu_populate (NMApplet *applet, GtkMenu *menu)
{
	GtkMenuShell *menu_shell;
	guint id;
	static gboolean icons_shown = FALSE;

	g_return_if_fail (applet != NULL);

	menu_shell = GTK_MENU_SHELL (menu);

	if (G_UNLIKELY (icons_shown == FALSE)) {
		GtkSettings *settings = gtk_widget_get_settings (GTK_WIDGET (menu_shell));

		/* We always want our icons displayed */
		if (settings)
			g_object_set (G_OBJECT (settings), "gtk-menu-images", TRUE, NULL);
		icons_shown = TRUE;
	}

	/* 'Enable Networking' item */
	applet->networking_enabled_item = gtk_check_menu_item_new_with_mnemonic (_("Enable _Networking"));
	id = g_signal_connect (applet->networking_enabled_item,
	                       "toggled",
	                       G_CALLBACK (nma_set_networking_enabled_cb),
	                       applet);
	applet->networking_enabled_toggled_id = id;
	gtk_menu_shell_append (menu_shell, applet->networking_enabled_item);

	/* 'Enable Wi-Fi' item */
	applet->wifi_enabled_item = gtk_check_menu_item_new_with_mnemonic (_("Enable _Wi-Fi"));
	id = g_signal_connect (applet->wifi_enabled_item,
	                       "toggled",
	                       G_CALLBACK (nma_set_wifi_enabled_cb),
	                       applet);
	applet->wifi_enabled_toggled_id = id;
	gtk_menu_shell_append (menu_shell, applet->wifi_enabled_item);

	/* 'Enable Mobile Broadband' item */
	applet->wwan_enabled_item = gtk_check_menu_item_new_with_mnemonic (_("Enable _Mobile Broadband"));
	id = g_signal_connect (applet->wwan_enabled_item,
	                       "toggled",
	                       G_CALLBACK (nma_set_wwan_enabled_cb),
	                       applet);
	applet->wwan_enabled_toggled_id = id;
	gtk_menu_shell_append (menu_shell, applet->wwan_enabled_item);

	nma_menu_add_separator_item (GTK_WIDGET (menu_shell));

	if (!INDICATOR_ENABLED (applet)) {
		/* Toggle notifications item */
		applet->notifications_enabled_item = gtk_check_menu_item_new_with_mnemonic (_("Enable all notifications"));
		id = g_signal_connect (applet->notifications_enabled_item,
			                   "toggled",
			                   G_CALLBACK (nma_set_notifications_enabled_cb),
			                   applet);
		applet->notifications_enabled_toggled_id = id;
		gtk_menu_shell_append (menu_shell, applet->notifications_enabled_item);

		nma_menu_add_notification_submenu(GTK_WIDGET (menu_shell), applet);

		nma_menu_add_separator_item (GTK_WIDGET (menu_shell));
	}

	/* 'Connection Information' item */
	applet->info_menu_item = gtk_menu_item_new_with_mnemonic (_("Connection _Information"));
	g_signal_connect_swapped (applet->info_menu_item,
	                          "activate",
	                          G_CALLBACK (applet_connection_info_cb),
	                          applet);
	gtk_menu_shell_append (menu_shell, applet->info_menu_item);

	/* 'Edit Connections...' item */
	applet->connections_menu_item = gtk_menu_item_new_with_mnemonic (_("Edit Connections…"));
	g_signal_connect (applet->connections_menu_item,
				   "activate",
				   G_CALLBACK (nma_edit_connections_cb),
				   applet);
	gtk_menu_shell_append (menu_shell, applet->connections_menu_item);

	/* Separator */
	nma_menu_add_separator_item (GTK_WIDGET (menu_shell));

	if (!INDICATOR_ENABLED (applet)) {
		/* About item */
		GtkWidget *menu_item;

		menu_item = gtk_menu_item_new_with_mnemonic (_("_About"));
		g_signal_connect_swapped (menu_item, "activate", G_CALLBACK (applet_about_dialog_show), applet);
		gtk_menu_shell_append (menu_shell, menu_item);
	}

	gtk_widget_show_all (GTK_WIDGET (menu_shell));
}

typedef struct {
	NMApplet *applet;
	NMDevice *device;
	NMConnection *connection;
} AppletMenuItemInfo;

static void
applet_menu_item_info_destroy (gpointer data, GClosure *closure)
{
	AppletMenuItemInfo *info = data;

	g_clear_object (&info->device);
	g_clear_object (&info->connection);

	g_slice_free (AppletMenuItemInfo, data);
}

static void
applet_menu_item_activate (GtkMenuItem *item, gpointer user_data)
{
	AppletMenuItemInfo *info = user_data;

	applet_menu_item_activate_helper (info->device,
	                                  info->connection,
	                                  "/",
	                                  info->applet,
	                                  user_data);
}

void
applet_add_connection_items (NMDevice *device,
                             const GPtrArray *connections,
                             gboolean sensitive,
                             NMConnection *active,
                             NMAAddActiveInactiveEnum flag,
                             GtkWidget *menu,
                             NMApplet *applet)
{
	int i;
	AppletMenuItemInfo *info;

	for (i = 0; i < connections->len; i++) {
		NMConnection *connection = NM_CONNECTION (connections->pdata[i]);
		GtkWidget *item;

		if (active == connection) {
			if ((flag & NMA_ADD_ACTIVE) == 0)
				continue;
		} else {
			if ((flag & NMA_ADD_INACTIVE) == 0)
				continue;
		}

		item = applet_new_menu_item_helper (connection, active, (flag & NMA_ADD_ACTIVE));
		gtk_widget_set_sensitive (item, sensitive);
		gtk_widget_show_all (item);

		info = g_slice_new0 (AppletMenuItemInfo);
		info->applet = applet;
		info->device = device ? g_object_ref (device) : NULL;
		info->connection = g_object_ref (connection);

		g_signal_connect_data (item, "activate",
		                       G_CALLBACK (applet_menu_item_activate),
		                       info,
		                       applet_menu_item_info_destroy, 0);

		gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	}
}

void
applet_add_default_connection_item (NMDevice *device,
                                    const char *label,
                                    gboolean sensitive,
                                    GtkWidget *menu,
                                    NMApplet *applet)
{
	AppletMenuItemInfo *info;
	GtkWidget *item;

	item = gtk_check_menu_item_new_with_label (label);
	gtk_widget_set_sensitive (GTK_WIDGET (item), sensitive);
	gtk_check_menu_item_set_draw_as_radio (GTK_CHECK_MENU_ITEM (item), TRUE);

	info = g_slice_new0 (AppletMenuItemInfo);
	info->applet = applet;
	info->device = g_object_ref (device);

	g_signal_connect_data (item, "activate",
	                       G_CALLBACK (applet_menu_item_activate),
	                       info,
	                       applet_menu_item_info_destroy, 0);

	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
}

static gboolean
applet_update_menu (gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);
	GList *children, *elt;
	GtkMenu *menu;

	if (INDICATOR_ENABLED (applet)) {
#ifdef WITH_APPINDICATOR
		menu = app_indicator_get_menu (applet->app_indicator);
		if (!menu) {
			menu = GTK_MENU (gtk_menu_new ());
			app_indicator_set_menu (applet->app_indicator, menu);
			g_signal_connect_swapped (menu, "show", G_CALLBACK (applet_start_wifi_scan), applet);
			g_signal_connect_swapped (menu, "hide", G_CALLBACK (applet_stop_wifi_scan), applet);

			/* Work around ubuntu libappindicator not emitting the above signals in runtime */
			g_signal_connect_swapped (menu, "show", G_CALLBACK (applet_workaround_show_cb), applet);
		}
#else
		g_return_val_if_reached (G_SOURCE_REMOVE);
#endif /* WITH_APPINDICATOR */
	} else {
		menu = GTK_MENU (applet->menu);
		if (!menu) {
			/* Menu not open */
			goto out;
		}
	}

	/* Clear all entries */
	children = gtk_container_get_children (GTK_CONTAINER (menu));
	for (elt = children; elt; elt = g_list_next (elt))
		gtk_container_remove (GTK_CONTAINER (menu), GTK_WIDGET (elt->data));
	g_list_free (children);

	/* Update the menu */
	if (INDICATOR_ENABLED (applet)) {
		nma_menu_show_cb (GTK_WIDGET (menu), applet);
		nma_menu_add_separator_item (GTK_WIDGET (menu));
		nma_context_menu_populate (applet, menu);
		nma_context_menu_update (applet);
	} else
		nma_menu_show_cb (GTK_WIDGET (menu), applet);

out:
	applet->update_menu_id = 0;
	return G_SOURCE_REMOVE;
}

void
applet_schedule_update_menu (NMApplet *applet)
{
	if (!applet->update_menu_id)
		applet->update_menu_id = g_idle_add (applet_update_menu, applet);
}

/*****************************************************************************/

static void
foo_set_icon (NMApplet *applet, guint32 layer, GdkPixbuf *pixbuf, const char *icon_name)
{
	gs_unref_object GdkPixbuf *pixbuf_free = NULL;

	g_return_if_fail (layer == ICON_LAYER_LINK || layer == ICON_LAYER_VPN);

#ifdef WITH_APPINDICATOR
	if (INDICATOR_ENABLED (applet)) {
		/* FIXME: We rely on the fact that VPN icon gets drawn later and therefore
		 * wins but we cannot currently set a combined pixmap made of both the link
		 * icon and the VPN icon.
		 */
		if (icon_name == NULL && layer == ICON_LAYER_LINK)
			icon_name = "nm-no-connection";
		if (icon_name != NULL && g_strcmp0 (app_indicator_get_icon (applet->app_indicator), icon_name) != 0)
			app_indicator_set_icon_full (applet->app_indicator, icon_name, applet->tip);
		return;
	}
#endif  /* WITH_APPINDICATOR */

	/* Load the pixbuf by icon name */
	if (icon_name /*&& !pixbuf*/) //alex: force icon_name using
		pixbuf = nma_tray_icon_check_and_load (icon_name, applet);

	/* Ignore setting of the same icon as is already displayed */
	if (applet->icon_layers[layer] == pixbuf)
		return;

	g_clear_object (&applet->icon_layers[layer]);

	if (pixbuf)
		applet->icon_layers[layer] = g_object_ref (pixbuf);

	if (applet->icon_layers[0]) {
		int i;

		pixbuf = applet->icon_layers[0];

		for (i = ICON_LAYER_LINK + 1; i <= ICON_LAYER_MAX; i++) {
			GdkPixbuf *top = applet->icon_layers[i];

			if (!top)
				continue;

			if (!pixbuf_free)
				pixbuf = pixbuf_free = gdk_pixbuf_copy (pixbuf);

			gdk_pixbuf_composite (top, pixbuf, 0, 0, gdk_pixbuf_get_width (top),
			                      gdk_pixbuf_get_height (top),
			                      0, 0, 1.0, 1.0,
			                      GDK_INTERP_NEAREST, 255);
		}
	} else
		pixbuf = nma_tray_icon_check_and_load ("nm-no-connection", applet); //alex

	gtk_status_icon_set_from_pixbuf (applet->status_icon, pixbuf);
}

NMRemoteConnection *
applet_get_exported_connection_for_device (NMDevice *device, NMApplet *applet)
{
	const GPtrArray *active_connections;
	int i;

	active_connections = nm_client_get_active_connections (applet->nm_client);
	for (i = 0; active_connections && (i < active_connections->len); i++) {
		NMActiveConnection *active;
		NMRemoteConnection *connection;
		const GPtrArray *devices;

		active = g_ptr_array_index (active_connections, i);
		if (!active)
			continue;

		devices = nm_active_connection_get_devices (active);
		connection = nm_active_connection_get_connection (active);
		if (!devices || !connection)
			continue;

		if (!nm_g_ptr_array_contains (devices, device))
			continue;

		return connection;
	}
	return NULL;
}

static void
applet_common_device_state_changed (NMDevice *device,
                                    NMDeviceState new_state,
                                    NMDeviceState old_state,
                                    NMDeviceStateReason reason,
                                    NMApplet *applet)
{
	gboolean device_activating = FALSE, vpn_activating = FALSE;

	device_activating = applet_is_any_device_activating (applet);
	vpn_activating = applet_is_any_vpn_activating (applet);


	switch (new_state) {
	case NM_DEVICE_STATE_PREPARE:
	case NM_DEVICE_STATE_CONFIG:
	case NM_DEVICE_STATE_NEED_AUTH:
	case NM_DEVICE_STATE_IP_CONFIG:
		/* Be sure to turn animation timeout on here since the dbus signals
		 * for new active connections or devices might not have come through yet.
		 */
		device_activating = TRUE;
		break;
	case NM_DEVICE_STATE_ACTIVATED:
	default:
		break;
	}

	/* If there's an activating device but we're not animating, start animation.
	 * If we're animating, but there's no activating device or VPN, stop animating.
	 */
	if (device_activating || vpn_activating)
		start_animation_timeout (applet);
	else
		clear_animation_timeout (applet);
}

static void
foo_device_state_changed_cb (NMDevice *device,
                             NMDeviceState new_state,
                             NMDeviceState old_state,
                             NMDeviceStateReason reason,
                             gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);
	NMADeviceClass *dclass;

	dclass = get_device_class (device, applet);

	// Save the reason for informing the user later.
	applet->state_reason = reason;

	if (dclass && dclass->device_state_changed)
		dclass->device_state_changed (device, new_state, old_state, reason, applet);

	applet_common_device_state_changed (device, new_state, old_state, reason, applet);

	if (   dclass
	    && new_state == NM_DEVICE_STATE_ACTIVATED
	    && !g_settings_get_boolean (applet->gsettings, PREF_DISABLE_CONNECTED_NOTIFICATIONS)) {
		NMConnection *connection;
		char *str = NULL;

		connection = applet_find_active_connection_for_device (device, applet, NULL);
		if (connection) {
			str = g_strdup_printf (_("You are now connected to “%s”."),
			                       nm_connection_get_id (connection));
		}

		dclass->notify_connected (device, str, applet);
		g_free (str);
	}

	applet_schedule_update_icon (applet);
	applet_schedule_update_menu (applet);
}

static void
foo_device_added_cb (NMClient *client, NMDevice *device, gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);
	NMADeviceClass *dclass;

	dclass = get_device_class (device, applet);
	if (dclass && dclass->device_added)
		dclass->device_added (device, applet);

	g_signal_connect (device, "state-changed",
	                  G_CALLBACK (foo_device_state_changed_cb),
	                  user_data);

	foo_device_state_changed_cb	(device,
	                             nm_device_get_state (device),
	                             NM_DEVICE_STATE_UNKNOWN,
	                             NM_DEVICE_STATE_REASON_NONE,
	                             applet);
}

static void
foo_client_state_changed_cb (NMClient *client, GParamSpec *pspec, gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);

	switch (nm_client_get_state (client)) {
	case NM_STATE_DISCONNECTED:
		if (NM_DEVICE_STATE_REASON_IP_ADDRESS_DUPLICATE == applet->state_reason)
		{
			applet_do_notify (applet,
				_("NetworkManager has detected an IP address conflict."),
				_("Another computer on this network has the same IP address as this computer.\n"
				"Contact your network administrator for help resolving this issue.\n"
				"More details are available in the system event log."),
				"nm-no-connection",
				PREF_DISABLE_DISCONNECTED_NOTIFICATIONS);
		}
		else
		{
			applet_do_notify (applet, _("Disconnected"),
			                        _("The network connection has been disconnected."),
			                        "nm-no-connection",
			                        PREF_DISABLE_DISCONNECTED_NOTIFICATIONS);
		}
		break;
	default:
		break;
	}

	applet_schedule_update_icon (applet);
	applet_schedule_update_menu (applet);
}

static void
foo_device_removed_cb (NMClient *client, NMDevice *device, NMApplet *applet)
{
	applet_schedule_update_icon (applet);
	applet_schedule_update_menu (applet);
}

static void
foo_manager_running_cb (NMClient *client,
                        GParamSpec *pspec,
                        gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);

	if (nm_client_get_nm_running (client)) {
		g_debug ("NM appeared");
	} else {
		g_debug ("NM disappeared");
		clear_animation_timeout (applet);
	}

	applet_schedule_update_icon (applet);
	applet_schedule_update_menu (applet);
}

static void
vpn_state_changed (NMActiveConnection *connection,
                   GParamSpec *pspec,
                   gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);

	applet_schedule_update_icon (applet);
	applet_schedule_update_menu (applet);
}

#define VPN_STATE_ID_TAG "vpn-state-id"

static void
foo_active_connections_changed_cb (NMClient *client,
                                   GParamSpec *pspec,
                                   gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);
	const GPtrArray *active_list;
	int i;

	/* Track the state of new VPN connections */
	active_list = nm_client_get_active_connections (client);
	for (i = 0; active_list && (i < active_list->len); i++) {
		NMActiveConnection *candidate = NM_ACTIVE_CONNECTION (g_ptr_array_index (active_list, i));
		guint id;

		if (   !NM_IS_VPN_CONNECTION (candidate)
		    || g_object_get_data (G_OBJECT (candidate), VPN_STATE_ID_TAG))
			continue;

		/* Start/stop animation when the AC state changes ... */
		id = g_signal_connect (G_OBJECT (candidate), "state-changed",
		                       G_CALLBACK (vpn_active_connection_state_changed), applet);
		/* ... and also update icon/tooltip when the VPN state changes */
		g_signal_connect (G_OBJECT (candidate), "notify::vpn-state",
		                  G_CALLBACK (vpn_state_changed), applet);

		g_object_set_data (G_OBJECT (candidate), VPN_STATE_ID_TAG, GUINT_TO_POINTER (id));
	}

	applet_schedule_update_icon (applet);
	applet_schedule_update_menu (applet);
}

static void
foo_manager_permission_changed (NMClient *client,
                                NMClientPermission permission,
                                NMClientPermissionResult result,
                                gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);

	if (permission <= NM_CLIENT_PERMISSION_LAST)
		applet->permissions[permission] = result;
}

static void
foo_wireless_enabled_changed_cb (NMClient *client, GParamSpec *pspec, NMApplet *applet)
{
	applet_schedule_update_icon (applet);
	applet_schedule_update_menu (applet);
}

static gboolean
foo_set_initial_state (gpointer data)
{
	NMApplet *applet = NM_APPLET (data);
	const GPtrArray *devices;
	int i;

	devices = nm_client_get_devices (applet->nm_client);
	for (i = 0; devices && (i < devices->len); i++)
		foo_device_added_cb (applet->nm_client, NM_DEVICE (g_ptr_array_index (devices, i)), applet);

	foo_active_connections_changed_cb (applet->nm_client, NULL, applet);

	applet_schedule_update_icon (applet);

	return FALSE;
}

static void
foo_client_setup (NMApplet *applet)
{
	NMClientPermission perm;

	applet->nm_client = nm_client_new (NULL, NULL);
	if (!applet->nm_client)
		return;

	g_signal_connect (applet->nm_client, "notify::state",
	                  G_CALLBACK (foo_client_state_changed_cb),
	                  applet);
	g_signal_connect (applet->nm_client, "notify::active-connections",
	                  G_CALLBACK (foo_active_connections_changed_cb),
	                  applet);
	g_signal_connect (applet->nm_client, "device-added",
	                  G_CALLBACK (foo_device_added_cb),
	                  applet);
	if (INDICATOR_ENABLED (applet)) {
		g_signal_connect (applet->nm_client, "device-removed",
		                  G_CALLBACK (foo_device_removed_cb),
		                  applet);
	}
	g_signal_connect (applet->nm_client, "notify::manager-running",
	                  G_CALLBACK (foo_manager_running_cb),
	                  applet);

	g_signal_connect (applet->nm_client, "permission-changed",
	                  G_CALLBACK (foo_manager_permission_changed),
	                  applet);

	g_signal_connect (applet->nm_client, "notify::wireless-enabled",
	                  G_CALLBACK (foo_wireless_enabled_changed_cb),
	                  applet);

	g_signal_connect (applet->nm_client, "notify::wwan-enabled",
	                  G_CALLBACK (foo_wireless_enabled_changed_cb),
	                  applet);

	/* Initialize permissions - the initial 'permission-changed' signal is emitted from NMClient constructor, and thus not caught */
	for (perm = NM_CLIENT_PERMISSION_NONE + 1; perm <= NM_CLIENT_PERMISSION_LAST; perm++) {
		applet->permissions[perm] = nm_client_get_permission_result (applet->nm_client, perm);
	}

	if (nm_client_get_nm_running (applet->nm_client))
		g_idle_add (foo_set_initial_state, applet);

	applet_schedule_update_icon (applet);
}

#if WITH_WWAN

static void
mm1_name_owner_changed_cb (GDBusObjectManagerClient *mm1,
                           GParamSpec *pspec,
                           NMApplet *applet)
{
	gchar *name_owner;

	name_owner = g_dbus_object_manager_client_get_name_owner (mm1);
	applet->mm1_running = !!name_owner;
	g_free (name_owner);

	if (applet->mm1_running) {
		const GPtrArray *devices;
		NMADeviceClass *dclass;
		NMDevice *device;
		int i;

		devices = nm_client_get_devices (applet->nm_client);
		for (i = 0; devices && (i < devices->len); i++) {
			device = NM_DEVICE (g_ptr_array_index (devices, i));
			if (NM_IS_DEVICE_MODEM (device)) {
				dclass = get_device_class (device, applet);
				if (dclass && dclass->device_added)
					dclass->device_added (device, applet);

				applet_schedule_update_icon (applet);
				applet_schedule_update_menu (applet);
			}
		}
	}
}

static void
mm_new_ready (GDBusConnection *connection,
              GAsyncResult *res,
              NMApplet *applet)
{
	GError *error = NULL;

	applet->mm1 = mm_manager_new_finish (res, &error);
	if (applet->mm1) {
		/* We've got our MM proxy, now check whether the ModemManager
		 * is really running and usable */
		g_signal_connect (applet->mm1,
		                  "notify::name-owner",
		                  G_CALLBACK (mm1_name_owner_changed_cb),
		                  applet);
		mm1_name_owner_changed_cb (G_DBUS_OBJECT_MANAGER_CLIENT (applet->mm1), NULL, applet);
	} else {
		g_warning ("Error connecting to D-Bus: %s", error->message);
		g_clear_error (&error);
	}
}

static void
mm1_client_setup (NMApplet *applet)
{
	GDBusConnection *system_bus;
	GError *error = NULL;

	system_bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
	if (system_bus) {
		mm_manager_new (system_bus,
		                G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
		                NULL,
		                (GAsyncReadyCallback) mm_new_ready,
		                applet);
		g_object_unref (system_bus);
	} else {
		g_warning ("Error connecting to system D-Bus: %s", error->message);
		g_clear_error (&error);
	}
}

#endif /* WITH_WWAN */

static void
applet_common_get_device_icon (NMDeviceState state,
                               GdkPixbuf **out_pixbuf,
                               char **out_icon_name,
                               NMApplet *applet)
{
	int stage = -1;

	switch (state) {
	case NM_DEVICE_STATE_PREPARE:
		stage = 0;
		break;
	case NM_DEVICE_STATE_CONFIG:
	case NM_DEVICE_STATE_NEED_AUTH:
		stage = 1;
		break;
	case NM_DEVICE_STATE_IP_CONFIG:
		stage = 2;
		break;
	default:
		break;
	}

	if (stage >= 0) {
		char *name = g_strdup_printf ("nm-stage%02d-connecting%02d", stage + 1, applet->animation_step + 1);

		if (out_pixbuf)
			*out_pixbuf = nm_g_object_ref (nma_icon_check_and_load (name, applet));
		if (out_icon_name)
			*out_icon_name = name;
		else
			g_free (name);

		applet->animation_step++;
		if (applet->animation_step >= NUM_CONNECTING_FRAMES)
			applet->animation_step = 0;
	}
}

static char *
get_tip_for_device_state (NMDevice *device,
                          NMDeviceState state,
                          NMConnection *connection)
{
	char *tip = NULL;
	const char *id = NULL;

	id = nm_device_get_iface (device);
	if (connection)
		id = nm_connection_get_id (connection);

	switch (state) {
	case NM_DEVICE_STATE_PREPARE:
	case NM_DEVICE_STATE_CONFIG:
		tip = g_strdup_printf (_("Preparing network connection “%s”…"), id);
		break;
	case NM_DEVICE_STATE_NEED_AUTH:
		tip = g_strdup_printf (_("User authentication required for network connection “%s”…"), id);
		break;
	case NM_DEVICE_STATE_IP_CONFIG:
		tip = g_strdup_printf (_("Requesting a network address for “%s”…"), id);
		break;
	case NM_DEVICE_STATE_ACTIVATED:
		tip = g_strdup_printf (_("Network connection “%s” active"), id);
		break;
	default:
		break;
	}

	return tip;
}

static void
applet_get_device_icon_for_state (NMApplet *applet,
                                  GdkPixbuf **out_pixbuf,
                                  char **out_icon_name,
                                  char **out_tip)
{
	NMActiveConnection *active;
	NMDevice *device = NULL;
	NMDeviceState state = NM_DEVICE_STATE_UNKNOWN;
	NMADeviceClass *dclass;

	g_assert (out_pixbuf && out_icon_name && out_tip);
	g_assert (!*out_pixbuf && !*out_icon_name && !*out_tip);

	// FIXME: handle multiple device states here

	/* First show the best activating device's state */
	active = applet_get_best_activating_connection (applet, &device);
	if (!active || !device) {
		/* If there aren't any activating devices, then show the state of
		 * the default active connection instead.
		 */
		active = applet_get_default_active_connection (applet, &device, TRUE);
		if (!active || !device)
			goto out;
	}

	state = nm_device_get_state (device);

	dclass = get_device_class (device, applet);
	if (dclass) {
		NMConnection *connection;
		const char *icon_name = NULL;

		connection = applet_find_active_connection_for_device (device, applet, NULL);

		dclass->get_icon (device, state, connection, out_pixbuf, &icon_name, out_tip, applet);

		if (!*out_pixbuf && icon_name)
			*out_pixbuf = nm_g_object_ref (nma_icon_check_and_load (icon_name, applet));
		*out_icon_name = g_strdup (icon_name);
		if (!*out_tip)
			*out_tip = get_tip_for_device_state (device, state, connection);
		if (icon_name || *out_pixbuf)
			return;
	}

out:
	applet_common_get_device_icon (state, out_pixbuf, out_icon_name, applet);
}

static char *
get_tip_for_vpn (NMActiveConnection *active, NMVpnConnectionState state, NMApplet *applet)
{
	char *tip = NULL;
	const char *id = NULL;

	id = nm_active_connection_get_id (active);
	if (!id)
		return NULL;

	switch (state) {
	case NM_VPN_CONNECTION_STATE_CONNECT:
	case NM_VPN_CONNECTION_STATE_PREPARE:
		tip = g_strdup_printf (_("Starting VPN connection “%s”…"), id);
		break;
	case NM_VPN_CONNECTION_STATE_NEED_AUTH:
		tip = g_strdup_printf (_("User authentication required for VPN connection “%s”…"), id);
		break;
	case NM_VPN_CONNECTION_STATE_IP_CONFIG_GET:
		tip = g_strdup_printf (_("Requesting a VPN address for “%s”…"), id);
		break;
	case NM_VPN_CONNECTION_STATE_ACTIVATED:
		tip = g_strdup_printf (_("VPN connection active"));
		break;
	default:
		break;
	}

	return tip;
}

static gboolean
applet_update_icon (gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);
	gs_unref_object GdkPixbuf *pixbuf = NULL;
	NMState state;
	const char *icon_name, *dev_tip;
	char *vpn_tip = NULL;
	gs_free char *icon_name_free = NULL;
	gs_free char *dev_tip_free = NULL;
	NMVpnConnectionState vpn_state = NM_VPN_CONNECTION_STATE_UNKNOWN;
	gboolean nm_running;
	NMActiveConnection *active_vpn = NULL;

	applet->update_icon_id = 0;

	nm_running = nm_client_get_nm_running (applet->nm_client);

	/* Handle device state first */

	state = nm_client_get_state (applet->nm_client);
	if (!nm_running)
		state = NM_STATE_UNKNOWN;

#ifdef WITH_APPINDICATOR
	if (INDICATOR_ENABLED (applet))
		app_indicator_set_status (applet->app_indicator, nm_running ? APP_INDICATOR_STATUS_ACTIVE : APP_INDICATOR_STATUS_PASSIVE);
	else
#endif  /* WITH_APPINDICATOR */
	{
		gtk_status_icon_set_visible (applet->status_icon, applet->visible);
	}

	switch (state) {
	case NM_STATE_UNKNOWN:
	case NM_STATE_ASLEEP:
		icon_name = "nm-no-connection";
		dev_tip = _("Networking disabled");
		break;
	case NM_STATE_DISCONNECTED:
		icon_name = "nm-no-connection";
		dev_tip = _("No network connection");
		break;
	default:
		applet_get_device_icon_for_state (applet, &pixbuf, &icon_name_free, &dev_tip_free);
		icon_name = icon_name_free;
		dev_tip = dev_tip_free;
		if (!pixbuf && state == NM_STATE_CONNECTED_GLOBAL) {
			icon_name = g_strdup ("nm-device-wired");
			pixbuf = g_object_ref (nma_icon_check_and_load (icon_name, applet));
		}
		break;
	}

	foo_set_icon (applet, ICON_LAYER_LINK, pixbuf, icon_name);

	icon_name = NULL;
	g_clear_pointer (&icon_name_free, g_free);

	/* VPN state next */
	active_vpn = applet_get_active_vpn_connection (applet, &vpn_state);
	if (active_vpn) {
		switch (vpn_state) {
		case NM_VPN_CONNECTION_STATE_ACTIVATED:
			icon_name = "nm-vpn-active-lock";
#ifdef WITH_APPINDICATOR
			if (INDICATOR_ENABLED (applet))
				icon_name = icon_name_free = g_strdup_printf ("%s-secure", app_indicator_get_icon (applet->app_indicator));
#endif /* WITH_APPINDICATOR */
			break;
		case NM_VPN_CONNECTION_STATE_PREPARE:
		case NM_VPN_CONNECTION_STATE_NEED_AUTH:
		case NM_VPN_CONNECTION_STATE_CONNECT:
		case NM_VPN_CONNECTION_STATE_IP_CONFIG_GET:
			icon_name = icon_name_free = g_strdup_printf ("nm-vpn-connecting%02d", applet->animation_step + 1);
			applet->animation_step++;
			if (applet->animation_step >= NUM_VPN_CONNECTING_FRAMES)
				applet->animation_step = 0;
			break;
		default:
			break;
		}

		vpn_tip = get_tip_for_vpn (active_vpn, vpn_state, applet);
		if (vpn_tip && dev_tip) {
			char *tmp;

			tmp = g_strdup_printf ("%s\n%s", dev_tip, vpn_tip);
			g_free (vpn_tip);
			vpn_tip = tmp;
		}
	}
	foo_set_icon (applet, ICON_LAYER_VPN, NULL, icon_name);

	/* update tooltip */
	g_free (applet->tip);
	if (vpn_tip)
		applet->tip = vpn_tip;
	else if (dev_tip == dev_tip_free) {
		applet->tip = dev_tip_free;
		dev_tip_free = NULL;
	} else
		applet->tip = g_strdup (dev_tip);

	if (applet->status_icon) {
			gtk_status_icon_set_tooltip_text (applet->status_icon, applet->tip);
			gtk_status_icon_set_title (applet->status_icon, applet->tip);
	}

	return FALSE;
}

void
applet_schedule_update_icon (NMApplet *applet)
{
	if (!applet->update_icon_id)
		applet->update_icon_id = g_idle_add (applet_update_icon, applet);
}

/*****************************************************************************/

static SecretsRequest *
applet_secrets_request_new (size_t totsize,
                            NMConnection *connection,
                            gpointer request_id,
                            const char *setting_name,
                            const char **hints,
                            guint32 flags,
                            AppletAgentSecretsCallback callback,
                            gpointer callback_data,
                            NMApplet *applet)
{
	SecretsRequest *req;

	g_return_val_if_fail (totsize >= sizeof (SecretsRequest), NULL);
	g_return_val_if_fail (connection != NULL, NULL);

	req = g_malloc0 (totsize);
	req->totsize = totsize;
	req->connection = g_object_ref (connection);
	req->reqid = request_id;
	req->setting_name = g_strdup (setting_name);
	req->hints = g_strdupv ((char **) hints);
	req->flags = flags;
	req->callback = callback;
	req->callback_data = callback_data;
	req->applet = applet;
	return req;
}

void
applet_secrets_request_set_free_func (SecretsRequest *req,
                                      SecretsRequestFreeFunc free_func)
{
	req->free_func = free_func;
}

void
applet_secrets_request_complete (SecretsRequest *req,
                                 GVariant *settings,
                                 GError *error)
{
	req->callback (req->applet->agent, error ? NULL : settings, error, req->callback_data);
}

void
applet_secrets_request_complete_setting (SecretsRequest *req,
                                         const char *setting_name,
                                         GError *error)
{
	NMSetting *setting;
	GVariant *secrets_dict = NULL;

	if (setting_name && !error) {
		setting = nm_connection_get_setting_by_name (req->connection, setting_name);
		if (setting) {
			secrets_dict = nm_connection_to_dbus (req->connection, NM_CONNECTION_SERIALIZE_ALL);
			if (!secrets_dict) {
				g_set_error (&error,
				             NM_SECRET_AGENT_ERROR,
				             NM_SECRET_AGENT_ERROR_FAILED,
				             "%s.%d (%s): failed to hash setting '%s'.",
				             __FILE__, __LINE__, __func__, setting_name);
			}
		} else {
			g_set_error (&error,
			             NM_SECRET_AGENT_ERROR,
			             NM_SECRET_AGENT_ERROR_FAILED,
			             "%s.%d (%s): unhandled setting '%s'",
			             __FILE__, __LINE__, __func__, setting_name);
		}
	}

	req->callback (req->applet->agent, secrets_dict, error, req->callback_data);
}

void
applet_secrets_request_free (SecretsRequest *req)
{
	g_return_if_fail (req != NULL);

	if (req->free_func)
		req->free_func (req);

	req->applet->secrets_reqs = g_slist_remove (req->applet->secrets_reqs, req);

	g_object_unref (req->connection);
	g_free (req->setting_name);
	g_strfreev (req->hints);
	memset (req, 0, req->totsize);
	g_free (req);
}

static void
get_existing_secrets_cb (NMSecretAgentOld *agent,
                         NMConnection *connection,
                         GVariant *secrets,
                         GError *secrets_error,
                         gpointer user_data)
{
	SecretsRequest *req = user_data;
	NMADeviceClass *dclass;
	GError *error = NULL;

	if (secrets)
		nm_connection_update_secrets (connection, req->setting_name, secrets, NULL);
	else
		nm_connection_clear_secrets (connection);

	dclass = get_device_class_from_connection (connection, req->applet);
	g_assert (dclass);

	/* Let the device class handle secrets */
	if (!dclass->get_secrets (req, &error)) {
		g_warning ("%s:%d - %s", __func__, __LINE__, error ? error->message : "(unknown)");
		applet_secrets_request_complete (req, NULL, error);
		applet_secrets_request_free (req);
		g_error_free (error);
	}
	/* Otherwise success; wait for the secrets callback */
}

static void
applet_agent_get_secrets_cb (AppletAgent *agent,
                             gpointer request_id,
                             NMConnection *connection,
                             const char *setting_name,
                             const char **hints,
                             guint32 flags,
                             AppletAgentSecretsCallback callback,
                             gpointer callback_data,
                             gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);
	NMSettingConnection *s_con;
	NMADeviceClass *dclass;
	GError *error = NULL;
	SecretsRequest *req = NULL;

	s_con = nm_connection_get_setting_connection (connection);
	g_return_if_fail (s_con != NULL);

	/* VPN secrets get handled a bit differently */
	if (!strcmp (nm_setting_connection_get_connection_type (s_con), NM_SETTING_VPN_SETTING_NAME)) {
		req = applet_secrets_request_new (applet_vpn_request_get_secrets_size (),
		                                  connection,
		                                  request_id,
		                                  setting_name,
		                                  hints,
		                                  flags,
		                                  callback,
		                                  callback_data,
		                                  applet);
		if (!applet_vpn_request_get_secrets (req, &error))
			goto error;

		applet->secrets_reqs = g_slist_prepend (applet->secrets_reqs, req);
		return;
	}

	dclass = get_device_class_from_connection (connection, applet);
	if (!dclass) {
		error = g_error_new (NM_SECRET_AGENT_ERROR,
		                     NM_SECRET_AGENT_ERROR_FAILED,
		                     "%s.%d (%s): device type unknown",
		                     __FILE__, __LINE__, __func__);
		goto error;
	}

	if (!dclass->get_secrets) {
		error = g_error_new (NM_SECRET_AGENT_ERROR,
		                     NM_SECRET_AGENT_ERROR_NO_SECRETS,
		                     "%s.%d (%s): no secrets found",
		                     __FILE__, __LINE__, __func__);
		goto error;
	}

	g_assert (dclass->secrets_request_size);
	req = applet_secrets_request_new (dclass->secrets_request_size,
	                                  connection,
	                                  request_id,
	                                  setting_name,
	                                  hints,
	                                  flags,
	                                  callback,
	                                  callback_data,
	                                  applet);
	applet->secrets_reqs = g_slist_prepend (applet->secrets_reqs, req);

	/* Get existing secrets, if any */
	nm_secret_agent_old_get_secrets (NM_SECRET_AGENT_OLD (applet->agent),
	                                 connection,
	                                 setting_name,
	                                 hints,
	                                 NM_SECRET_AGENT_GET_SECRETS_FLAG_NONE,
	                                 get_existing_secrets_cb,
	                                 req);
	return;

error:
	g_warning ("%s", error->message);
	callback (agent, NULL, error, callback_data);
	g_error_free (error);

	if (req)
		applet_secrets_request_free (req);
}

static void
applet_agent_cancel_secrets_cb (AppletAgent *agent,
                                gpointer request_id,
                                gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);
	GSList *iter, *next;

	for (iter = applet->secrets_reqs; iter; iter = next) {
		SecretsRequest *req = iter->data;

		next = g_slist_next (iter);

		if (req->reqid == request_id) {
			/* cancel and free this password request */
			applet_secrets_request_free (req);
			break;
		}
	}
}

/*****************************************************************************/

static void nma_icons_free (NMApplet *applet)
{
	guint i;

	g_return_if_fail (NM_IS_APPLET (applet));

	for (i = 0; i <= ICON_LAYER_MAX; i++)
		g_clear_object (&applet->icon_layers[i]);
}

GdkPixbuf *  //alex
nma_tray_icon_check_and_load (const char *name, NMApplet *applet)
{
	GError *error = NULL;
	GdkPixbuf *icon;

	g_assert (name != NULL);
	g_assert (applet != NULL);

	/* icon already loaded successfully */
	if (g_hash_table_lookup_extended (applet->icon_cache_tray, name, NULL, (gpointer) &icon))
		return icon;

	/* Try to load the icon; if the load fails, log the problem, and set
	 * the icon to the fallback icon if requested.
	 */
	if (!applet->icon_theme_tray)
        return nma_icon_check_and_load (name, applet);

	if (!(icon = gtk_icon_theme_load_icon (applet->icon_theme_tray, name, applet->icon_size, GTK_ICON_LOOKUP_FORCE_SIZE, &error))) {
		g_warning ("failed to load icon \"%s\": %s", name, error->message);
		g_clear_error (&error);
        return nma_icon_check_and_load (name, applet); //alex
		//icon = nm_g_object_ref (applet->fallback_icon);
	}

	g_hash_table_insert (applet->icon_cache_tray, g_strdup (name), icon);

	return icon;
}

GdkPixbuf *
nma_icon_check_and_load (const char *name, NMApplet *applet)
{
	GError *error = NULL;
	GdkPixbuf *icon;
	int scale;

	g_assert (name != NULL);
	g_assert (applet != NULL);

	/* icon already loaded successfully */
	if (g_hash_table_lookup_extended (applet->icon_cache, name, NULL, (gpointer) &icon))
		return icon;

	scale = gdk_window_get_scale_factor (gdk_get_default_root_window ());

	/* Try to load the icon; if the load fails, log the problem, and set
	 * the icon to the fallback icon if requested.
	 */
	if (!(icon = gtk_icon_theme_load_icon_for_scale (applet->icon_theme, name, applet->icon_size, scale, GTK_ICON_LOOKUP_FORCE_SIZE, &error))) {
		g_warning ("failed to load icon \"%s\": %s", name, error->message);
		g_clear_error (&error);
		icon = nm_g_object_ref (applet->fallback_icon);
	}

	g_hash_table_insert (applet->icon_cache, g_strdup (name), icon);

	return icon;
}

#include "fallback-icon.h"

static void
nma_icons_reload (NMApplet *applet)
{
	GError *error = NULL;
	gs_unref_object GdkPixbufLoader *loader = NULL;

	g_return_if_fail (applet->icon_size > 0);

	g_hash_table_remove_all (applet->icon_cache_tray); //alex
	nma_icons_free (applet);

	if (applet->fallback_icon)
		return;

	loader = gdk_pixbuf_loader_new_with_type ("png", &error);
	if (!loader)
		goto error;

	if (!gdk_pixbuf_loader_write (loader,
	                              fallback_icon_data,
	                              sizeof (fallback_icon_data),
	                              &error))
		goto error;

	if (!gdk_pixbuf_loader_close (loader, &error))
		goto error;

	applet->fallback_icon = nm_g_object_ref (gdk_pixbuf_loader_get_pixbuf (loader));
	g_warn_if_fail (applet->fallback_icon);
	return;

error:
	g_warning ("failed loading default-icon: %s", error->message);
	g_clear_error (&error);
}

static void nma_icon_theme_changed (GtkIconTheme *icon_theme, NMApplet *applet)
{
	nma_icons_reload (applet);
	applet_schedule_update_icon (applet);
}

//alex: xsettings processing -----------------------------------------------------------
static void _xsettings_notify_cb(const char *name, XSettingsAction action, XSettingsSetting *setting, void *data)
{
  NMApplet *applet = (NMApplet *)data;

  if (!data || !setting || !name) return;
  if (action != XSETTINGS_ACTION_NEW && action != XSETTINGS_ACTION_CHANGED) return;
  if (strcmp(name, "Fly/TrayIconThemeName")!=0 || setting->type != XSETTINGS_TYPE_STRING) return;

  //g_debug("xsettings_notify_cb, theme=%s new=%s",applet->icon_theme_tray_name,setting->data.v_string);
  if (applet->icon_theme_tray_name) {
    if (strcmp(applet->icon_theme_tray_name, setting->data.v_string) == 0) return;
    free(applet->icon_theme_tray_name); applet->icon_theme_tray_name=NULL;
  }

  if (!applet->icon_theme_tray) applet->icon_theme_tray = gtk_icon_theme_new();
  applet->icon_theme_tray_name = strdup(setting->data.v_string);
  gtk_icon_theme_set_custom_theme (applet->icon_theme_tray,applet->icon_theme_tray_name);

  nma_icon_theme_changed(applet->icon_theme_tray, applet);
}

static GdkFilterReturn _xsettings_manager_filter (GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
  if (!data) return GDK_FILTER_CONTINUE;

  XEvent *xev = xevent;
  NMApplet  *applet = (NMApplet *)data;

  //g_debug("xsettings_manager_filterb xev=%p applet=%p",xev,data);
  if (applet->xsettings_client) xsettings_client_process_event(applet->xsettings_client, xev);

  return GDK_FILTER_CONTINUE;
}

static void _xsettings_watch_cb(Window window, Bool is_start, long mask, void  *data)
{
  if (!data) return;
/*
  GdkWindow * gw = NULL;
  gw = gdk_x11_window_foreign_new_for_display (gdk_display_get_default(), window);
  gw = gdk_x11_window_lookup_for_display(gdk_display_get_default(), window);
  g_debug("xsettings_watch_cb win=%d gw=%p is_start=%d data=%p",window,gw,is_start,data);
*/
  if (is_start) gdk_window_add_filter(NULL/*gw*/, _xsettings_manager_filter, data);
}
//-------------------------------------------------------------------------------------------

static void nma_icons_init (NMApplet *applet)
{
	XSettingsSetting *s;
    GdkDisplay* gd = gdk_display_get_default();
    Display* d = GDK_DISPLAY_XDISPLAY(gd);

    gdk_x11_display_set_window_scale(gd,1); //alex: workaround for invisibale tray icon when Gdk/WindowScalingFactor > 1

	gboolean path_appended;

	if (applet->icon_theme) {
		g_signal_handlers_disconnect_by_func (applet->icon_theme,
		                                      G_CALLBACK (nma_icon_theme_changed),
		                                      applet);
		g_object_unref (G_OBJECT (applet->icon_theme));
	}

	if (applet->status_icon)
		applet->icon_theme = gtk_icon_theme_get_for_screen (gtk_status_icon_get_screen (applet->status_icon));
	else
		applet->icon_theme = gtk_icon_theme_get_default ();

	//alex: load tray icon theme-------------------------------------------------------------
    //g_debug("nma_icons_init: get white or back or none theme from xsettings");
    if (applet->icon_theme_tray)      { g_clear_object (&applet->icon_theme_tray); applet->icon_theme_tray =NULL; }
    if (applet->icon_theme_tray_name) { free(applet->icon_theme_tray_name); applet->icon_theme_tray_name=NULL; }
    if (applet->xsettings_client)     { xsettings_client_destroy(applet->xsettings_client); applet->xsettings_client=NULL; }

    applet->xsettings_client = xsettings_client_new(d, DefaultScreen(d), _xsettings_notify_cb, _xsettings_watch_cb, (void *)applet );
    if (applet->xsettings_client &&
        xsettings_client_get_setting(applet->xsettings_client,"Fly/TrayIconThemeName",&s)==XSETTINGS_SUCCESS &&
        s->data.v_string && *(s->data.v_string))
    {
          applet->icon_theme_tray_name = strdup(s->data.v_string);
          applet->icon_theme_tray = gtk_icon_theme_new();
          gtk_icon_theme_set_custom_theme (applet->icon_theme_tray,applet->icon_theme_tray_name);
    }
    //----------------------------------------------------------------------------------

	/* If not done yet, append our search path */
	path_appended = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (applet->icon_theme),
	                                 "NMAIconPathAppended"));
	if (path_appended == FALSE) {
		gtk_icon_theme_append_search_path (applet->icon_theme, ICONDIR);
		g_object_set_data (G_OBJECT (applet->icon_theme),
		                   "NMAIconPathAppended",
		                   GINT_TO_POINTER (TRUE));
	}

	g_signal_connect (applet->icon_theme, "changed", G_CALLBACK (nma_icon_theme_changed), applet);

	nma_icons_reload (applet);
}

static void
status_icon_screen_changed_cb (GtkStatusIcon *icon,
                               GParamSpec *pspec,
                               NMApplet *applet)
{
	nma_icons_init (applet);
}

static gboolean
status_icon_size_changed_cb (GtkStatusIcon *icon,
                             gint size,
                             NMApplet *applet)
{
	g_debug ("%s(): status icon size %d requested", __func__, size);

	/* icon_size may be 0 if for example the panel hasn't given us any space
	 * yet.  We'll get resized later, but for now just load the 16x16 icons.
	 */
	if (size > 0) {
		if (size < 22)
			applet->icon_size = 16;
		else if (size < 24)
			applet->icon_size = 22;
		else if (size < 32)
			applet->icon_size = 24;
		else if (size < 48)
			applet->icon_size = 32;
		else
			applet->icon_size = size;
	}
	else {
		applet->icon_size = 16;
		g_warn_if_fail (size == 0);
	}

	nma_icons_reload (applet);

	applet_schedule_update_icon (applet);

	return TRUE;
}

static void
status_icon_activate_cb (GtkStatusIcon *icon, NMApplet *applet)
{
	/* Have clicking on the applet act also as acknowledgement
	 * of the notification.
	 */
	g_application_withdraw_notification (G_APPLICATION (applet), "nm-applet");

	applet_start_wifi_scan (applet, NULL);

	/* Kill any old menu */
	if (applet->menu)
		g_object_unref (applet->menu);

	/* And make a fresh new one */
	applet->menu = gtk_menu_new ();
	/* Sink the ref so we can explicitly destroy the menu later */
	g_object_ref_sink (G_OBJECT (applet->menu));

	gtk_container_set_border_width (GTK_CONTAINER (applet->menu), 0);
	g_signal_connect (applet->menu, "show", G_CALLBACK (nma_menu_show_cb), applet);
	g_signal_connect (applet->menu, "deactivate", G_CALLBACK (nma_menu_deactivate_cb), applet);

	/* Display the new menu */
	gtk_menu_popup (GTK_MENU (applet->menu), NULL, NULL,
	                gtk_status_icon_position_menu, icon,
	                1, gtk_get_current_event_time ());
}

static void
status_icon_popup_menu_cb (GtkStatusIcon *icon,
                           guint button,
                           guint32 activate_time,
                           NMApplet *applet)
{
	/* Have clicking on the applet act also as acknowledgement
	 * of the notification.
	 */
	g_application_withdraw_notification (G_APPLICATION (applet), "nm-applet");

	nma_context_menu_update (applet);
	gtk_menu_popup (GTK_MENU (applet->context_menu), NULL, NULL,
			gtk_status_icon_position_menu, icon,
			button, activate_time);
}

static gboolean
setup_widgets (NMApplet *applet)
{
	GtkMenu *menu;

#ifdef WITH_APPINDICATOR
	if (with_appindicator) {
		applet->app_indicator = app_indicator_new ("nm-applet",
		                                           "nm-no-connection",
		                                           APP_INDICATOR_CATEGORY_SYSTEM_SERVICES);
		if (!applet->app_indicator)
			return FALSE;
		app_indicator_set_title(applet->app_indicator, _("Network"));
		applet_schedule_update_menu (applet);
	}
#endif  /* WITH_APPINDICATOR */

	/* Fall back to status icon if indicator isn't enabled or built */
	if (!INDICATOR_ENABLED (applet)) {
		applet->status_icon = gtk_status_icon_new ();

		if (shell_debug)
			gtk_status_icon_set_name (applet->status_icon, "adsfasdfasdfadfasdf");

		g_signal_connect (applet->status_icon, "notify::screen",
				  G_CALLBACK (status_icon_screen_changed_cb), applet);
		g_signal_connect (applet->status_icon, "size-changed",
				  G_CALLBACK (status_icon_size_changed_cb), applet);
		g_signal_connect (applet->status_icon, "activate",
				  G_CALLBACK (status_icon_activate_cb), applet);
		g_signal_connect (applet->status_icon, "popup-menu",
				  G_CALLBACK (status_icon_popup_menu_cb), applet);

		menu = GTK_MENU (gtk_menu_new ());
		nma_context_menu_populate (applet, menu);
		applet->context_menu = GTK_WIDGET (menu);
		if (!applet->context_menu)
			return FALSE;
	}

	return TRUE;
}

static void
applet_embedded_cb (GObject *object, GParamSpec *pspec, gpointer user_data)
{
	gboolean embedded = gtk_status_icon_is_embedded (GTK_STATUS_ICON (object));

	g_debug ("applet now %s the notification area",
	         embedded ? "embedded in" : "removed from");
}

static void
register_agent (NMApplet *applet)
{
	GError *error = NULL;

	g_return_if_fail (!applet->agent);

	applet->agent = applet_agent_new (&error);
	if (!applet->agent) {
		if (!error)
			error = g_error_new (NM_SECRET_AGENT_ERROR,
			                     NM_SECRET_AGENT_ERROR_FAILED,
			                     "Could not register secret agent");
		g_warning ("%s", error->message);
		g_error_free (error);
		return;
	}
	g_assert (applet->agent);
	g_signal_connect (applet->agent, APPLET_AGENT_GET_SECRETS,
	                  G_CALLBACK (applet_agent_get_secrets_cb), applet);
	g_signal_connect (applet->agent, APPLET_AGENT_CANCEL_SECRETS,
	                  G_CALLBACK (applet_agent_cancel_secrets_cb), applet);

	if (INDICATOR_ENABLED (applet)) {
		/* Watch for new connections */
		g_signal_connect_swapped (applet->nm_client, NM_CLIENT_CONNECTION_ADDED,
		                          G_CALLBACK (applet_schedule_update_menu),
		                          applet);
	}
}

static void
applet_gsettings_show_changed (GSettings *settings,
                               gchar *key,
                               gpointer user_data)
{
	NMApplet *applet = NM_APPLET (user_data);

	g_return_if_fail (NM_IS_APPLET(applet));
	g_return_if_fail (key != NULL);

	applet->visible = g_settings_get_boolean (settings, key);

	if (applet->status_icon)
		gtk_status_icon_set_visible (applet->status_icon, applet->visible);
}

/****************************************************************/

static void
applet_enable_pref (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
	g_settings_set_boolean (NM_APPLET (user_data)->gsettings,
	                        g_variant_get_string (parameter, NULL),
	                        TRUE);
}

static GActionEntry app_entries[] =
{
	{ "enable-pref", applet_enable_pref, "s" },
};

static void
applet_activate (GApplication *app, gpointer user_data)
{
	/* Nothing to do, but glib requires this handler */
}

static void
applet_startup (GApplication *app, gpointer user_data)
{
	NMApplet *applet = NM_APPLET (app);
	gs_free_error GError *error = NULL;

	g_set_application_name (_("NetworkManager Applet"));
	gtk_window_set_default_icon_name ("network-workgroup");

	g_action_map_add_action_entries (G_ACTION_MAP (app), app_entries,
	                                 G_N_ELEMENTS (app_entries), app);

	applet->info_dialog_ui = gtk_builder_new ();

	if (!gtk_builder_add_from_resource (applet->info_dialog_ui, "/org/freedesktop/network-manager-applet/info.ui", &error)) {
		g_warning ("Could not load info dialog UI file: %s", error->message);
		g_application_quit (app);
		return;
	}

	applet->gsettings = g_settings_new (APPLET_PREFS_SCHEMA);
	applet->visible = g_settings_get_boolean (applet->gsettings, PREF_SHOW_APPLET);
	g_signal_connect (applet->gsettings, "changed::show-applet",
	                  G_CALLBACK (applet_gsettings_show_changed), applet);

	foo_client_setup (applet);

	/* Load pixmaps and create applet widgets */
	if (!setup_widgets (applet)) {
		g_warning ("Could not initialize applet widgets.");
		g_application_quit (app);
		return;
	}
	g_assert (INDICATOR_ENABLED (applet) || applet->status_icon);

	applet->icon_cache = g_hash_table_new_full (g_str_hash,
	                                            g_str_equal,
	                                            g_free,
	                                            nm_g_object_unref);

	applet->icon_cache_tray = g_hash_table_new_full (g_str_hash, //alex
	                                            	g_str_equal,
	                                            	g_free,
	                                            	nm_g_object_unref);
    applet->icon_theme_tray     =NULL;
    applet->icon_theme_tray_name=NULL;
    applet->xsettings_client    =NULL;


	nma_icons_init (applet);

	/* Initialize device classes */
	applet->ethernet_class = applet_device_ethernet_get_class (applet);
	g_assert (applet->ethernet_class);

	applet->wifi_class = applet_device_wifi_get_class (applet);
	g_assert (applet->wifi_class);

#if WITH_WWAN
	applet->broadband_class = applet_device_broadband_get_class (applet);
	g_assert (applet->broadband_class);
#endif

	applet->bt_class = applet_device_bt_get_class (applet);
	g_assert (applet->bt_class);

#if WITH_WWAN
	mm1_client_setup (applet);
#endif

	if (applet->status_icon) {
		/* Track embedding to help debug issues where user has removed the
		 * notification area applet from the panel, and thus nm-applet too.
		 */
		g_signal_connect (applet->status_icon, "notify::embedded",
			              G_CALLBACK (applet_embedded_cb), NULL);
		applet_embedded_cb (G_OBJECT (applet->status_icon), NULL, NULL);
	}

	if (with_agent)
		register_agent (applet);

	g_application_hold (G_APPLICATION (applet));
}

static void finalize (GObject *object)
{
	NMApplet *applet = NM_APPLET (object);

	g_slice_free (NMADeviceClass, applet->ethernet_class);
	g_slice_free (NMADeviceClass, applet->wifi_class);
#if WITH_WWAN
	g_slice_free (NMADeviceClass, applet->broadband_class);
#endif
	g_slice_free (NMADeviceClass, applet->bt_class);

	nm_clear_g_source (&applet->update_icon_id);
	nm_clear_g_source (&applet->wifi_scan_id);

#ifdef WITH_APPINDICATOR
	g_clear_object (&applet->app_indicator);
#endif /* WITH_APPINDICATOR */
	nm_clear_g_source (&applet->update_menu_id);

	g_clear_object (&applet->status_icon);
	g_clear_object (&applet->menu);
	g_clear_pointer (&applet->icon_cache, g_hash_table_destroy);

	//alex: destoy tray icon theme and xsettings
	g_clear_pointer (&applet->icon_cache_tray, g_hash_table_destroy);
	if (applet->icon_theme_tray_name) free(applet->icon_theme_tray_name);
	if (applet->xsettings_client) xsettings_client_destroy(applet->xsettings_client);
	g_clear_object (&applet->icon_theme_tray);

	g_clear_object (&applet->fallback_icon);
	g_free (applet->tip);
	nma_icons_free (applet);

	while (g_slist_length (applet->secrets_reqs))
		applet_secrets_request_free ((SecretsRequest *) applet->secrets_reqs->data);

	g_clear_object (&applet->info_dialog_ui);
	g_clear_object (&applet->gsettings);
	g_clear_object (&applet->nm_client);

#if WITH_WWAN
	g_clear_object (&applet->mm1);
#endif

	g_clear_object (&applet->agent);

	G_OBJECT_CLASS (nma_parent_class)->finalize (object);
}

static void nma_init (NMApplet *applet)
{
	applet->icon_size = 16;

	g_signal_connect (applet, "startup", G_CALLBACK (applet_startup), NULL);
	g_signal_connect (applet, "activate", G_CALLBACK (applet_activate), NULL);
}

static void nma_class_init (NMAppletClass *klass)
{
	GObjectClass *oclass = G_OBJECT_CLASS (klass);

	oclass->finalize = finalize;
}
