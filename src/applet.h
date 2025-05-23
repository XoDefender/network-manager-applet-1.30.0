// SPDX-License-Identifier: GPL-2.0+
/* NetworkManager Applet -- allow user control over networking
 *
 * Copyright 2004 - 2014 Red Hat, Inc.
 * Copyright 2005 - 2008 Novell, Inc.
 */

#ifndef APPLET_H
#define APPLET_H

#include <string.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <net/ethernet.h>

#ifdef WITH_APPINDICATOR
#if USE_AYATANA_INDICATORS
#include <libayatana-appindicator/app-indicator.h>
#else
#include <libappindicator/app-indicator.h>
#endif
#endif

#include <NetworkManager.h>

#include "applet-agent.h"

#if WITH_WWAN
#include <libmm-glib.h>
#endif

//alex
#include <xsettings-client.h>

#define NM_TYPE_APPLET              (nma_get_type())
#define NM_APPLET(object)           (G_TYPE_CHECK_INSTANCE_CAST((object), NM_TYPE_APPLET, NMApplet))
#define NM_APPLET_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST((klass), NM_TYPE_APPLET, NMAppletClass))
#define NM_IS_APPLET(object)        (G_TYPE_CHECK_INSTANCE_TYPE((object), NM_TYPE_APPLET))
#define NM_IS_APPLET_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE((klass), NM_TYPE_APPLET))
#define NM_APPLET_GET_CLASS(object) (G_TYPE_INSTANCE_GET_CLASS((object), NM_TYPE_APPLET, NMAppletClass))

#ifdef WITH_APPINDICATOR
#define INDICATOR_ENABLED(a) ((a)->app_indicator)
#else
#define INDICATOR_ENABLED(a) (FALSE)
#endif  /* WITH_APPINDICATOR */

typedef struct {
	GApplicationClass parent_class;
} NMAppletClass;

#define APPLET_PREFS_SCHEMA                       "org.gnome.nm-applet"
#define PREF_DISABLE_CONNECTED_NOTIFICATIONS      "disable-connected-notifications"
#define PREF_DISABLE_DISCONNECTED_NOTIFICATIONS   "disable-disconnected-notifications"
#define PREF_DISABLE_VPN_NOTIFICATIONS            "disable-vpn-notifications"
#define PREF_DISABLE_WIFI_CREATE                  "disable-wifi-create"
#define PREF_SUPPRESS_WIFI_NETWORKS_AVAILABLE     "suppress-wireless-networks-available"
#define PREF_SHOW_APPLET                          "show-applet"

#define PREF_DISABLE_REASON_DEVICE_DISCONNECTED     "disable-device-disconnected-notification"
#define PREF_DISABLE_REASON_SERVICE_STOPPED         "disable-service-stopped-notification"
#define PREF_DISABLE_REASON_IP_CONFIG_INVALID       "disable-ip-config-invalid-notification"
#define PREF_DISABLE_REASON_CONNECT_TIMEOUT         "disable-connect-timeout-notification"
#define PREF_DISABLE_REASON_SERVICE_START_TIMEOUT   "disable-service-start-timeout-notification"
#define PREF_DISABLE_REASON_SERVICE_START_FAILED    "disable-service-start-failed-notification"
#define PREF_DISABLE_REASON_NO_SECRETS              "disable-no-secrets-notification"
#define PREF_DISABLE_REASON_LOGIN_FAILED            "disable-login-failed-notification"
#define PREF_DISABLE_REASON_USER_DISCONNECTED       "disable-user-disconnected-notification"

#define ICON_LAYER_LINK                           0
#define ICON_LAYER_VPN                            1
#define ICON_LAYER_MAX                            ICON_LAYER_VPN

typedef struct NMADeviceClass NMADeviceClass;

/*
 * Applet instance data
 *
 */
typedef struct {
	GApplication parent;

	NMClient *nm_client;
	AppletAgent *agent;

	GSettings *gsettings;

	// For detect an IP address conflict
	NMDeviceStateReason state_reason;

#if WITH_WWAN
	MMManager *mm1;
	gboolean   mm1_running;
#endif

	gboolean visible;

	/* Permissions */
	NMClientPermissionResult permissions[NM_CLIENT_PERMISSION_LAST + 1];

	/* Device classes */
	NMADeviceClass *ethernet_class;
	NMADeviceClass *wifi_class;
#if WITH_WWAN
	NMADeviceClass *broadband_class;
#endif
	NMADeviceClass *bt_class;

	/* Data model elements */
	guint           update_icon_id;
	char *          tip;

	/* Animation stuff */
	int             animation_step;
	guint           animation_id;
#define NUM_CONNECTING_FRAMES 11
#define NUM_VPN_CONNECTING_FRAMES 14

	GtkIconTheme *  icon_theme;
	GtkIconTheme *  icon_theme_tray; //alex
	char * icon_theme_tray_name; //alex
 	GHashTable *    icon_cache;
	GHashTable *    icon_cache_tray; //alex
 	GdkPixbuf *     fallback_icon;
 	int             icon_size;

    XSettingsClient *xsettings_client; //alex

	/* Active status icon pixbufs */
	GdkPixbuf *     icon_layers[ICON_LAYER_MAX + 1];

	/* Direct UI elements */
#ifdef WITH_APPINDICATOR
	AppIndicator *  app_indicator;
	bool            app_indicator_show_signal_received;
#endif
	guint           update_menu_id;

	GtkStatusIcon * status_icon;

	GtkWidget *     menu;
	GtkWidget *     context_menu;

	GtkWidget *     notifications_enabled_item;
	guint           notifications_enabled_toggled_id;
	GtkMenu   *     notifications_menu;

	GtkWidget *     networking_enabled_item;
	guint           networking_enabled_toggled_id;
	GtkWidget *     wifi_enabled_item;
	guint           wifi_enabled_toggled_id;
	GtkWidget *     wwan_enabled_item;
	guint           wwan_enabled_toggled_id;

	GtkWidget *     info_menu_item;
	GtkWidget *     connections_menu_item;

	GtkBuilder *    info_dialog_ui;

	/* Tracker objects for secrets requests */
	GSList *        secrets_reqs;

	guint           wifi_scan_id;
} NMApplet;

typedef void (*AppletNewAutoConnectionCallback) (NMConnection *connection,
                                                 gboolean created,
                                                 gboolean canceled,
                                                 gpointer user_data);

typedef struct _SecretsRequest SecretsRequest;
typedef void (*SecretsRequestFreeFunc) (SecretsRequest *req);

struct _SecretsRequest {
	size_t totsize;
	gpointer reqid;
	char *setting_name;
	char **hints;
	guint32 flags;
	NMApplet *applet;
	AppletAgentSecretsCallback callback;
	gpointer callback_data;

	NMConnection *connection;

	/* Class-specific stuff */
	SecretsRequestFreeFunc free_func;
};

void applet_secrets_request_set_free_func (SecretsRequest *req,
                                           SecretsRequestFreeFunc free_func);
void applet_secrets_request_complete (SecretsRequest *req,
                                      GVariant *settings,
                                      GError *error);
void applet_secrets_request_complete_setting (SecretsRequest *req,
                                              const char *setting_name,
                                              GError *error);
void applet_secrets_request_free (SecretsRequest *req);

struct NMADeviceClass {
	gboolean       (*new_auto_connection)  (NMDevice *device,
	                                        gpointer user_data,
	                                        AppletNewAutoConnectionCallback callback,
	                                        gpointer callback_data);

	gboolean       (*add_menu_item)        (NMDevice *device,
	                                        gboolean multiple_devices,
	                                        const GPtrArray *connections,
	                                        NMConnection *active,
	                                        GtkWidget *menu,
	                                        NMApplet *applet);

	void           (*device_added)         (NMDevice *device, NMApplet *applet);

	void           (*device_state_changed) (NMDevice *device,
	                                        NMDeviceState new_state,
	                                        NMDeviceState old_state,
	                                        NMDeviceStateReason reason,
	                                        NMApplet *applet);
	void           (*notify_connected)     (NMDevice *device,
	                                        const char *msg,
	                                        NMApplet *applet);

	/* Device class is expected to pass a *referenced* pixbuf, which will
	 * be unrefed by the icon code.  This allows the device class to create
	 * a composited pixbuf if necessary and pass the reference to the caller.
	 */
	void           (*get_icon)             (NMDevice *device,
	                                        NMDeviceState state,
	                                        NMConnection *connection,
	                                        GdkPixbuf **out_pixbuf,
	                                        const char **out_icon_name,
	                                        char **tip,
	                                        NMApplet *applet);

	size_t         secrets_request_size;
	gboolean       (*get_secrets)          (SecretsRequest *req,
	                                        GError **error);
};

GType nma_get_type (void);

NMApplet *nm_applet_new (void);

void applet_schedule_update_icon (NMApplet *applet);
void applet_schedule_update_menu (NMApplet *applet);

NMClient *applet_get_settings (NMApplet *applet);

GPtrArray *applet_get_all_connections (NMApplet *applet);

gboolean nma_menu_device_check_unusable (NMDevice *device);

GtkWidget * nma_menu_device_get_menu_item (NMDevice *device,
                                           NMApplet *applet,
                                           const char *unavailable_msg);

void applet_menu_item_activate_helper (NMDevice *device,
                                       NMConnection *connection,
                                       const char *specific_object,
                                       NMApplet *applet,
                                       gpointer dclass_data);

void applet_menu_item_disconnect_helper (NMDevice *device,
                                         NMApplet *applet);

void applet_menu_item_add_complex_separator_helper (GtkWidget *menu,
                                                    NMApplet *applet,
                                                    const gchar* label);

GtkWidget*
applet_menu_item_create_device_item_helper (NMDevice *device,
                                            NMApplet *applet,
                                            const gchar *text);

NMRemoteConnection *applet_get_exported_connection_for_device (NMDevice *device, NMApplet *applet);

NMDevice *applet_get_device_for_connection (NMApplet *applet, NMConnection *connection);

void applet_do_notify (NMApplet *applet,
                       const char *title,
                       const char *body,
                       const char *icon_name,
                       const char *pref);

GtkWidget * applet_new_menu_item_helper (NMConnection *connection,
                                         NMConnection *active,
                                         gboolean add_active);

GdkPixbuf * nma_icon_check_and_load (const char *name,
                                     NMApplet *applet);
GdkPixbuf * nma_tray_icon_check_and_load (const char *name,
                                     NMApplet *applet);

gboolean applet_wifi_connect_to_hidden_network (NMApplet *applet);
gboolean applet_wifi_create_wifi_network (NMApplet *applet);
gboolean applet_wifi_can_create_wifi_network (NMApplet *applet);

typedef enum {
	NMA_ADD_ACTIVE   = 1,
	NMA_ADD_INACTIVE = 2,
} NMAAddActiveInactiveEnum;

void applet_add_connection_items (NMDevice *device,
                                  const GPtrArray *connections,
                                  gboolean sensitive,
                                  NMConnection *active,
                                  NMAAddActiveInactiveEnum flag,
                                  GtkWidget *menu,
                                  NMApplet *applet);

void applet_add_default_connection_item (NMDevice *device,
                                         const char *label,
                                         gboolean sensitive,
                                         GtkWidget *menu,
                                         NMApplet *applet);

#endif
