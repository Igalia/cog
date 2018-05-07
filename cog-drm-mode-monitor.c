/*
 * cog-drm-mode-monitor.c
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-mode-monitor.h"
#include "cog-drm-mode-monitor.h"
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <libudev.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef COG_DRM_MODE_MONITOR_SETTLING_DELAY
#define COG_DRM_MODE_MONITOR_SETTLING_DELAY 0.5
#endif /* !COG_DRM_MODE_MONITOR_SETTLING_DELAY */


/* Allow using g_auto* with libudev structures. */
typedef struct udev udev_t;
typedef struct udev_device udev_device_t;
typedef struct udev_monitor udev_monitor_t;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (udev_t, udev_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (udev_device_t, udev_device_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (udev_monitor_t, udev_monitor_unref)

/* Ditto for libdrm types. */
typedef drmModeCrtc* CogDrmCrtcArray;
typedef drmModeConnector* CogDrmConnectorArray;

static void cog_drm_crct_array_free (CogDrmCrtcArray *crtcs);
static void cog_drm_connector_array_free (CogDrmConnectorArray *connectors);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (drmModeFB, drmModeFreeFB)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (drmModeRes, drmModeFreeResources)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (drmModeCrtc, drmModeFreeCrtc)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CogDrmCrtcArray, cog_drm_crct_array_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CogDrmConnectorArray, cog_drm_connector_array_free)


struct _CogDrmModeMonitor
{
    GObject            parent;
    CogModeMonitorInfo mode_info;
    GFile             *file;
    char              *path;  /* Caches result of g_file_get_path(). */
    int                fd;
    udev_t            *udev;
    udev_monitor_t    *udev_mon;
    GTimer            *timer;
    guint              idle_tag;
    guint              source_tag;
};


G_DEFINE_QUARK (CogDrmModeMonitorError, cog_drm_mode_monitor_error)


static CogDrmCrtcArray*
cog_drm_crtc_array_read (int fd, drmModeRes *resources)
{
    g_assert_cmpint (fd, >=, 0);
    g_assert_nonnull (resources);

    g_autoptr(CogDrmCrtcArray) crtcs = g_new0 (drmModeCrtc*, resources->count_crtcs + 1);
    for (unsigned i = 0; i < resources->count_crtcs; i++) {
        if (!(crtcs[i] = drmModeGetCrtc (fd, resources->crtcs[i])))
            break;
    }
    return g_steal_pointer (&crtcs);
}

static void
cog_drm_crct_array_free (CogDrmCrtcArray *crtcs)
{
    if (crtcs) {
        for (unsigned i = 0; crtcs[i]; i++)
            drmModeFreeCrtc (crtcs[i]);
        g_free (crtcs);
    }
}


static CogDrmConnectorArray*
cog_drm_connector_array_read (int                 fd,
                              drmModeRes         *resources,
                              drmModeConnector* (*readConnector) (int, uint32_t))
{
    g_assert_cmpint (fd, >=, 0);
    g_assert_nonnull (resources);

    g_autoptr(CogDrmConnectorArray) connectors = g_new0 (drmModeConnector*, resources->count_connectors + 1);
    for (unsigned i = 0; i < resources->count_connectors; i++) {
        if (!(connectors[i] = (*readConnector) (fd, resources->connectors[i])))
            break;
    }
    return g_steal_pointer (&connectors);
}


static void
cog_drm_connector_array_free (CogDrmConnectorArray *connectors)
{
    if (connectors) {
        for (unsigned i = 0; connectors[i]; i++)
            drmModeFreeConnector (connectors[i]);
        g_free (connectors);
    }
}


static const CogModeMonitorInfo*
cog_drm_mode_monitor_get_info (CogModeMonitor *monitor)
{
    g_assert (COG_IS_DRM_MODE_MONITOR (monitor));
    return &(COG_DRM_MODE_MONITOR (monitor)->mode_info);
}


static void
cog_mode_monitor_interface_init (CogModeMonitorInterface *iface)
{
    iface->get_info = cog_drm_mode_monitor_get_info;
}


G_DEFINE_TYPE_WITH_CODE (CogDrmModeMonitor,
                         cog_drm_mode_monitor,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (COG_TYPE_MODE_MONITOR,
                                                cog_mode_monitor_interface_init))

enum {
    PROP_0,
    PROP_MODE_ID,
    N_OVERRIDEN_PROPERTIES = PROP_MODE_ID,
    PROP_DEVICE_PATH,
    N_PROPERTIES
};

static GParamSpec *s_properties[N_PROPERTIES] = { NULL, };


static void
cog_drm_mode_monitor_get_property (GObject    *object,
                                   unsigned    prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
    CogDrmModeMonitor *monitor = COG_DRM_MODE_MONITOR (object);
    switch (prop_id) {
        case PROP_MODE_ID:
            g_value_set_string (value, monitor->mode_info.mode_id);
            break;
        case PROP_DEVICE_PATH:
            g_value_set_string (value, cog_drm_mode_monitor_get_device_path (monitor));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
cog_drm_mode_monitor_dispose (GObject *object)
{
    CogDrmModeMonitor *monitor = COG_DRM_MODE_MONITOR (object);

    if (monitor->source_tag) {
        g_source_remove (monitor->source_tag);
        monitor->source_tag = 0;
    }

    g_clear_pointer (&monitor->timer, g_timer_destroy);
    g_clear_pointer (&monitor->udev_mon, udev_monitor_unref);
    g_clear_pointer (&monitor->udev, udev_unref);
    
    if (monitor->fd >= 0) {
        g_autoptr(GError) error = NULL;
        if (g_close (monitor->fd, &error) && error)
            g_warning ("Error closing device '%s': %s", monitor->path, error->message);
        monitor->fd = -1;
    }

    g_clear_pointer (&monitor->mode_info.mode_id, g_free);
    g_clear_pointer (&monitor->path, g_free);
    g_clear_object (&monitor->file);

    G_OBJECT_CLASS (cog_drm_mode_monitor_parent_class)->dispose (object);
}


static void
cog_drm_mode_monitor_class_init (CogDrmModeMonitorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = cog_drm_mode_monitor_dispose;
    object_class->get_property = cog_drm_mode_monitor_get_property;
    
    CogModeMonitorInterface *mmon_iface =
        g_type_default_interface_ref (COG_TYPE_MODE_MONITOR);

    s_properties[PROP_DEVICE_PATH] =
        g_param_spec_string ("device-path",
                             "Device node path",
                             "Full path to the DRM/KSM device being monitored",
                             NULL,
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class,
                                       N_PROPERTIES - N_OVERRIDEN_PROPERTIES,
                                       s_properties + N_OVERRIDEN_PROPERTIES);

    s_properties[PROP_MODE_ID] =
        g_object_interface_find_property (mmon_iface, "mode-id");
    g_object_class_override_property (object_class,
                                      PROP_MODE_ID,
                                      "mode-id");

    g_type_default_interface_unref (mmon_iface);
}


static void
cog_drm_mode_monitor_init (CogDrmModeMonitor *monitor)
{
}


static void
cog_drm_mode_monitor_read_drm_resources (CogDrmModeMonitor *monitor)
{
    g_assert_nonnull (monitor);
    g_assert_cmpint (monitor->fd, >=, 0);

    g_autoptr(drmModeRes) resources = drmModeGetResources (monitor->fd);
    g_debug ("drm: %u crtcs, %u encoders, %u connectors",
             resources->count_crtcs,
             resources->count_encoders,
             resources->count_connectors);

    /*
     * Fetching information for connectors probes them, which will make it
     * possible to get accurate information about the resolution. (Ugh.)
     */
    g_autoptr(CogDrmConnectorArray) connectors =
        cog_drm_connector_array_read (monitor->fd, resources, drmModeGetConnector);

#ifndef NDEBUG
    for (unsigned i = 0; connectors[i]; i++) {
        g_debug ("drm: connector %u, id=%" PRIu32 ", %d modes",
                 i, connectors[i]->connector_id, connectors[i]->count_modes);
        for (unsigned j = 0; j < connectors[i]->count_modes; j++) {
            g_debug("drm:  mode '%s', %" PRIu32 "x%" PRIu32,
                    connectors[i]->modes[j].name,
                    connectors[i]->modes[j].hdisplay,
                    connectors[i]->modes[j].vdisplay);
        }
    }
#endif

    g_autoptr(CogDrmCrtcArray) crtcs = cog_drm_crtc_array_read (monitor->fd, resources);
    drmModeCrtc *crtc = NULL;

    for (unsigned i = 0; crtcs[i]; i++) {
        g_debug ("drm:  crtc %u: %p, id=%" PRIu32 " (%svalid mode, %sconnected)",
                 i, crtcs[i], crtcs[i]->crtc_id,
                 crtcs[i]->mode_valid ? "" : "in",
                 crtcs[i]->buffer_id ? "" : "dis");
        if (crtcs[i]->mode_valid && crtcs[i]->buffer_id) {
            if (crtc) {
                g_warning ("drm: More than one CRTC configured and connected. This is unsupported.");
            } else {
                crtc = crtcs[i];
            }
        }
    }

    if (!crtc)
        return;

#ifndef NDEBUG
    g_debug ("drm:  chosen: %p, id=%" PRIu32 ", buffer_id=%" PRIu32
             ", %" PRIu32 "x%" PRIu32 "+%" PRIu32 "+%" PRIu32,
             crtc, crtc->crtc_id, crtc->buffer_id,
             crtc->width, crtc->height, crtc->x, crtc->y);
    g_debug ("drm:  mode name='%s' clock=%" PRIu32 " vrefresh=%" PRIu32,
             crtc->mode.name, crtc->mode.clock, crtc->mode.vrefresh);
    g_debug ("drm:     hdisplay=%" PRIu16 " hsync_start=%" PRIu16 
             " hsync_end=%" PRIu16 " htotal=%" PRIu16 " hskew=%" PRIu16,
             crtc->mode.hdisplay, crtc->mode.hsync_start, crtc->mode.hsync_end,
             crtc->mode.htotal, crtc->mode.hskew);
    g_debug ("drm:     vdisplay=%" PRIu16 " vsync_start=%" PRIu16 
             " vsync_end=%" PRIu16 " vtotal=%" PRIu16 " vscan=%" PRIu16,
             crtc->mode.vdisplay, crtc->mode.vsync_start, crtc->mode.vsync_end,
             crtc->mode.vtotal, crtc->mode.vscan);
#endif

    g_autoptr(drmModeFB) fb = drmModeGetFB (monitor->fd, crtc->buffer_id);
    if (!fb)
        return;

    g_debug ("drm:  fb id=%" PRIu32 " mode=%" PRIu32 "x%" PRIu32 "@%" PRIu32
             " (depth=%" PRIu32 ") pitch/stride=%" PRIu32,
             fb->fb_id, fb->width, fb->height, fb->bpp, fb->depth, fb->pitch);

    g_autofree char *mode_id = g_strdup_printf ("%" PRIu32 "x%" PRIu32 "@%" PRIu32 "-%" PRIu32,
                                                fb->width, fb->height, fb->bpp, crtc->mode.vrefresh);

    if (g_strcmp0 (monitor->mode_info.mode_id, mode_id)) {
        /* Value was changed. Update and notify. */
        g_clear_pointer (&monitor->mode_info.mode_id, g_free);
        monitor->mode_info.mode_id = g_steal_pointer (&mode_id);
        monitor->mode_info.width = fb->width;
        monitor->mode_info.height = fb->height;
        g_object_notify_by_pspec (G_OBJECT (monitor), s_properties[PROP_MODE_ID]);
    }
}


static gboolean
on_drm_mode_monitor_idle_timer (gpointer user_data)
{
    g_autoptr(CogDrmModeMonitor) monitor = g_object_ref (COG_DRM_MODE_MONITOR (user_data));

    if (g_timer_elapsed (monitor->timer, NULL) < COG_DRM_MODE_MONITOR_SETTLING_DELAY)
        return TRUE;   /* Be invoked again. */

    /*
     * More than half a second passed: Pause and reset the timer to avoid
     * being called again while reading data from the device, and destroy
     * the timer and idle source afterwards.
     */
    g_timer_reset (monitor->timer);
    g_timer_stop (monitor->timer);

    cog_drm_mode_monitor_read_drm_resources (monitor);

    g_clear_pointer (&monitor->timer, g_timer_destroy);
    monitor->idle_tag = 0;
    return FALSE;  /* Remove the source. */
}


static gboolean
on_drm_mode_monitor_udev_fd_ready (int          fd,
                                   GIOCondition condition,
                                   void        *user_data)
{
    g_autoptr(CogDrmModeMonitor) monitor = g_object_ref (COG_DRM_MODE_MONITOR (user_data));
    g_autoptr(udev_device_t) device = udev_monitor_receive_device (monitor->udev_mon);

    if (!device) {
        g_warning ("Could not read udev_device_t from udev_monitor_t");
        goto beach;
    }

    if (monitor->timer) {
        g_timer_reset (monitor->timer);
    } else {
        monitor->timer = g_timer_new ();
    }

    if (!monitor->idle_tag) {
        monitor->idle_tag = g_idle_add (on_drm_mode_monitor_idle_timer, monitor);
    }

beach:
    return TRUE;  /* Get notified again. */
}


CogDrmModeMonitor*
cog_drm_mode_monitor_new (GFile   *file,
                          GError **error)
{
    if (!drmAvailable ()) {
        g_set_error_literal (error,
                             COG_DRM_MODE_MONITOR_ERROR,
                             COG_DRM_MODE_MONITOR_ERROR_UNAVAILABLE,
                             "DRM/KMS unavailable: No drivers loaded or no compatible harware attached");
        return NULL;
    }

    g_autoptr(CogDrmModeMonitor) monitor =
        g_object_new (COG_TYPE_DRM_MODE_MONITOR, NULL);
    monitor->file = file ? g_object_ref_sink (file) : g_file_new_for_path ("/dev/dri/card0");
    monitor->path = g_file_get_path (monitor->file);
    monitor->fd = -1;

    if (g_access (monitor->path, R_OK)) {
        g_set_error (error,
                     G_FILE_ERROR,
                     g_file_error_from_errno (errno),
                     "device '%s' is not readable",
                     monitor->path);
        return NULL;
    }

    if ((monitor->fd = g_open (monitor->path, O_RDONLY, 0)) < 0) {
        g_set_error (error,
                     G_FILE_ERROR,
                     g_file_error_from_errno (errno),
                     "cannot open '%s' for reading",
                     monitor->path);
        return NULL;
    }

    /* Read the current settings on creation, without emitting signals. */
    g_object_freeze_notify (G_OBJECT (monitor));
    cog_drm_mode_monitor_read_drm_resources (monitor);
    
    if (!(monitor->udev = udev_new ()) ||
        !(monitor->udev_mon = udev_monitor_new_from_netlink (monitor->udev, "udev")))
    {
        g_set_error_literal (error,
                             COG_DRM_MODE_MONITOR_ERROR,
                             COG_DRM_MODE_MONITOR_ERROR_UDEV,
                             "Cannot connect to udev");
        return NULL;
    }

    if (udev_monitor_filter_add_match_subsystem_devtype (monitor->udev_mon,
                                                         "drm",
                                                         "drm_minor") < 0) {
        g_set_error_literal (error,
                             COG_DRM_MODE_MONITOR_ERROR,
                             COG_DRM_MODE_MONITOR_ERROR_UDEV,
                             "Failed to set DRM udev filter");
        return NULL;
    }

    if (udev_monitor_enable_receiving (monitor->udev_mon) < 0) {
        g_set_error_literal (error,
                             COG_DRM_MODE_MONITOR_ERROR,
                             COG_DRM_MODE_MONITOR_ERROR_UDEV,
                             "Failed to enable reception of udev events");
        return NULL;
    }

    if (!g_unix_set_fd_nonblocking (udev_monitor_get_fd (monitor->udev_mon), TRUE, error))
        return NULL;

    monitor->source_tag = g_unix_fd_add (udev_monitor_get_fd (monitor->udev_mon),
                                         G_IO_IN,
                                         on_drm_mode_monitor_udev_fd_ready,
                                         monitor);

    g_object_thaw_notify (G_OBJECT (monitor));
    return g_steal_pointer (&monitor);
}


const char*
cog_drm_mode_monitor_get_device_path (CogDrmModeMonitor *monitor)
{
    g_return_val_if_fail (COG_IS_DRM_MODE_MONITOR (monitor), NULL);
    return monitor->path;
}


#ifdef COG_DRM_MODE_MONITOR_TEST_MAIN
#include <string.h>

static void
on_monitor_mode_changed (CogModeMonitor *monitor,
                         GParamSpec     *pspec,
                         void           *user_data)
{
    const CogModeMonitorInfo *info = cog_mode_monitor_get_info (monitor);
    g_print ("Monitor mode %" PRIu32 "x%" PRIu32 " (%s)\n",
             info->width, info->height, info->mode_id);
}

int
main (int argc, char **argv)
{
    const char *slash = strrchr (argv[0], '/');
    g_set_prgname (slash ? slash + 1 : argv[0]);

    g_autoptr(GError) error = NULL;
    g_autoptr(CogDrmModeMonitor) monitor = cog_drm_mode_monitor_new (NULL, &error);
    if (!monitor) {
        g_printerr ("%s: Cannot monitor: %s", g_get_prgname (), error->message);
        return 1;
    }

    on_monitor_mode_changed (COG_MODE_MONITOR (monitor), NULL, NULL);
    g_signal_connect (monitor, "notify::mode-id",
                      G_CALLBACK (on_monitor_mode_changed), NULL);

    g_autoptr(GMainLoop) mainloop = g_main_loop_new (NULL, FALSE);
    g_main_loop_run (mainloop);
    return 0;
}
#endif
