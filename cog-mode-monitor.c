/*
 * cog-mode-monitor.c
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-mode-monitor.h"


G_DEFINE_INTERFACE (CogModeMonitor, cog_mode_monitor, G_TYPE_OBJECT)


static const char*
cog_mode_monitor_default_get_mode_id (CogModeMonitor *monitor)
{
    g_assert_nonnull (monitor);
    const CogModeMonitorInfo *info = cog_mode_monitor_get_info (monitor);
    return info->mode_id;
}


static void
cog_mode_monitor_default_init (CogModeMonitorInterface *iface)
{
    iface->get_mode_id = cog_mode_monitor_default_get_mode_id;
    g_object_interface_install_property (iface,
                                         g_param_spec_string ("mode-id",
                                                              "Mode identifier",
                                                              "Unique identifier for the current screen mode",
                                                              NULL,
                                                              G_PARAM_READABLE |
                                                              G_PARAM_STATIC_STRINGS));
}


const CogModeMonitorInfo*
cog_mode_monitor_get_info (CogModeMonitor *monitor)
{
    g_return_val_if_fail (COG_IS_MODE_MONITOR (monitor), NULL);
    CogModeMonitorInterface *iface = COG_MODE_MONITOR_GET_IFACE (monitor);
    g_assert_nonnull (iface->get_info);
    return (*iface->get_info) (monitor);
}


const char*
cog_mode_monitor_get_mode_id (CogModeMonitor *monitor)
{
    g_return_val_if_fail (COG_IS_MODE_MONITOR (monitor), NULL);
    CogModeMonitorInterface *iface = COG_MODE_MONITOR_GET_IFACE (monitor);
    g_assert_nonnull (iface->get_mode_id);
    return (*iface->get_mode_id) (monitor);
}
