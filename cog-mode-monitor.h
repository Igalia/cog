/*
 * cog-mode-monitor.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef COG_MODE_MONITOR_H
#define COG_MODE_MONITOR_H

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include <glib-object.h>
#include <inttypes.h>

G_BEGIN_DECLS

#define COG_TYPE_MODE_MONITOR  (cog_mode_monitor_get_type ())

G_DECLARE_INTERFACE (CogModeMonitor, cog_mode_monitor, COG, MODE_MONITOR, GObject)

typedef struct _CogModeMonitorInfo CogModeMonitorInfo;

struct _CogModeMonitorInterface
{
    GTypeInterface g_iface;

    const CogModeMonitorInfo* (*get_info)    (CogModeMonitor *monitor);
    const char*               (*get_mode_id) (CogModeMonitor *monitor);
};

struct _CogModeMonitorInfo
{
    const char *mode_id;
    uint32_t    width;
    uint32_t    height;
};


const CogModeMonitorInfo *cog_mode_monitor_get_info    (CogModeMonitor *monitor);
const char               *cog_mode_monitor_get_mode_id (CogModeMonitor *monitor);

G_END_DECLS

#endif /* !COG_MODE_MONITOR_H */
