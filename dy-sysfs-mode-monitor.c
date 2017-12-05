/*
 * dy-sysfs-mode-monitor.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "dy-sysfs-mode-monitor.h"
#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>


#ifndef DY_SYSFS_MODE_MONITOR_RATE_LIMIT
#define DY_SYSFS_MODE_MONITOR_RATE_LIMIT 1000
#endif /* !DY_SYSFS_MODE_MONITOR_RATE_LIMIT */


struct _DySysfsModeMonitor
{
    GObject       parent;
    GFileMonitor *filemon;
    GFile        *file;
    char         *path;  /* Caches result of g_file_get_path(). */
    char         *mode;
};

G_DEFINE_TYPE (DySysfsModeMonitor, dy_sysfs_mode_monitor, G_TYPE_OBJECT)


enum {
    PROP_0,
    PROP_MODE,
    PROP_PATH,
    N_PROPERTIES
};

static GParamSpec *s_properties[N_PROPERTIES] = { NULL, };


static void
dy_sysfs_mode_monitor_get_property (GObject    *object,
                                    unsigned    prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
    DySysfsModeMonitor *monitor = DY_SYSFS_MODE_MONITOR (object);
    switch (prop_id) {
        case PROP_MODE:
            g_value_set_string (value, dy_sysfs_mode_monitor_get_mode (monitor));
            break;
        case PROP_PATH:
            g_value_set_string (value, dy_sysfs_mode_monitor_get_path (monitor));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
dy_sysfs_mode_monitor_dispose (GObject *object)
{
    DySysfsModeMonitor *monitor = DY_SYSFS_MODE_MONITOR (object);

    g_clear_object (&monitor->filemon);
    g_clear_object (&monitor->file);
    g_clear_pointer (&monitor->path, g_free);
    g_clear_pointer (&monitor->mode, g_free);

    G_OBJECT_CLASS (dy_sysfs_mode_monitor_parent_class)->dispose (object);
}


static void
dy_sysfs_mode_monitor_class_init (DySysfsModeMonitorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = dy_sysfs_mode_monitor_dispose;
    object_class->get_property = dy_sysfs_mode_monitor_get_property;

    s_properties[PROP_MODE] =
        g_param_spec_string ("mode",
                             "Current mode",
                             "Graphics mode for the device being monitored",
                             NULL,
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS);

    s_properties[PROP_PATH] =
        g_param_spec_string ("path",
                             "SysFS Path",
                             "SysFS path to the device being monitored",
                             NULL,
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class,
                                       N_PROPERTIES,
                                       s_properties);
}


static void
dy_sysfs_mode_monitor_init (DySysfsModeMonitor *monitor)
{
}


static gboolean
dy_sysfs_mode_monitor_read_mode_sync (DySysfsModeMonitor *monitor,
                                      GError            **error)
{
    g_assert_nonnull (monitor);

    g_autoptr(GInputStream) stream =
        G_INPUT_STREAM (g_file_read (monitor->file, NULL, error));
    if (!stream)
        return FALSE;

    g_autoptr(GDataInputStream) datain = g_data_input_stream_new (stream);
    g_data_input_stream_set_newline_type (datain, G_DATA_STREAM_NEWLINE_TYPE_LF);

    size_t line_len = 0;
    g_autoptr(GError) read_error = NULL;
    g_autofree char *line = g_data_input_stream_read_line (datain,
                                                           &line_len,
                                                           NULL,
                                                           &read_error);
    gboolean success = TRUE;
    if (!line && read_error) {
        if (error)
            *error = g_steal_pointer (&read_error);
        success = FALSE;
    }

    g_debug ("Monitor [%s] mode: %s -> %s", monitor->path, monitor->mode, line);
    if (g_strcmp0 (monitor->mode, line)) {
        /* Value has changed. Update and notify. */
        g_clear_pointer (&monitor->mode, g_free);
        monitor->mode = g_steal_pointer (&line);
        g_object_notify_by_pspec (G_OBJECT (monitor), s_properties[PROP_MODE]);
    }

    return success;
}


static void
on_file_monitor_changed (GFileMonitor       *filemon,
                         GFile              *file,
                         GFile              *other_file,
                         GFileMonitorEvent   event,
                         DySysfsModeMonitor *monitor)
{
    g_autoptr(GError) error = NULL;
    if (!dy_sysfs_mode_monitor_read_mode_sync (monitor, &error)) {
        g_warning ("Cannot read '%s': %s", 
                   dy_sysfs_mode_monitor_get_path (monitor),
                   error->message);
    }
}


DySysfsModeMonitor*
dy_sysfs_mode_monitor_new (GFile   *file,
                           GError **error)
{
    g_return_val_if_fail (file != NULL, NULL);

    g_autoptr(DySysfsModeMonitor) monitor =
        g_object_new (DY_TYPE_SYSFS_MODE_MONITOR, NULL);
    monitor->file = g_object_ref_sink (file);
    monitor->path = g_file_get_path (file);

    if (g_access (monitor->path, R_OK)) {
        g_set_error (error,
                     G_FILE_ERROR,
                     g_file_error_from_errno (errno),
                     "%s", monitor->path);
        return NULL;
    }

    /*
     * Do not emit property updates while reading the initial mode and
     * setting up the file monitor object, so client code doesn't get
     * spurious property change notifications.
     */
    g_object_freeze_notify (G_OBJECT (monitor));

    if (!dy_sysfs_mode_monitor_read_mode_sync (monitor, error) ||
        !(monitor->filemon = g_file_monitor_file (file,
                                                  G_FILE_MONITOR_NONE,
                                                  NULL,
                                                  error)))
        return NULL;

    g_file_monitor_set_rate_limit (monitor->filemon,
                                   DY_SYSFS_MODE_MONITOR_RATE_LIMIT);


    g_signal_connect (monitor->filemon,
                      "changed",
                      G_CALLBACK (on_file_monitor_changed),
                      monitor);

    g_object_thaw_notify (G_OBJECT (monitor));
    return g_steal_pointer (&monitor);
}


const char*
dy_sysfs_mode_monitor_get_mode (DySysfsModeMonitor *monitor)
{
    g_return_val_if_fail (monitor != NULL, NULL);
    return monitor->mode;
}


const char*
dy_sysfs_mode_monitor_get_path (DySysfsModeMonitor *monitor)
{
    g_return_val_if_fail (monitor != NULL, NULL);
    return monitor->path;
}
