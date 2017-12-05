/*
 * dy-mode-monitor.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "dy-mode-monitor.h"


G_DEFINE_INTERFACE (DyModeMonitor, dy_mode_monitor, G_TYPE_OBJECT)


static const char*
dy_mode_monitor_default_get_mode_id (DyModeMonitor *monitor)
{
    g_assert_nonnull (monitor);
    const DyModeMonitorInfo *info = dy_mode_monitor_get_info (monitor);
    return info->mode_id;
}


static void
dy_mode_monitor_default_init (DyModeMonitorInterface *iface)
{
    iface->get_mode_id = dy_mode_monitor_default_get_mode_id;
    g_object_interface_install_property (iface,
                                         g_param_spec_string ("mode-id",
                                                              "Mode identifier",
                                                              "Unique identifier for the current screen mode",
                                                              NULL,
                                                              G_PARAM_READABLE |
                                                              G_PARAM_STATIC_STRINGS));
}


const DyModeMonitorInfo*
dy_mode_monitor_get_info (DyModeMonitor *monitor)
{
    g_return_val_if_fail (DY_IS_MODE_MONITOR (monitor), NULL);
    DyModeMonitorInterface *iface = DY_MODE_MONITOR_GET_IFACE (monitor);
    g_assert_nonnull (iface->get_info);
    return (*iface->get_info) (monitor);
}


const char*
dy_mode_monitor_get_mode_id (DyModeMonitor *monitor)
{
    g_return_val_if_fail (DY_IS_MODE_MONITOR (monitor), NULL);
    DyModeMonitorInterface *iface = DY_MODE_MONITOR_GET_IFACE (monitor);
    g_assert_nonnull (iface->get_mode_id);
    return (*iface->get_mode_id) (monitor);
}
