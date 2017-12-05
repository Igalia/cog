/*
 * dy-mode-monitor.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef DY_MODE_MONITOR_H
#define DY_MODE_MONITOR_H

#if !(defined(DY_INSIDE_DINGHY__) && DY_INSIDE_DINGHY__)
# error "Do not include this header directly, use <dinghy.h> instead"
#endif

#include <glib-object.h>
#include <inttypes.h>

G_BEGIN_DECLS

#define DY_TYPE_MODE_MONITOR  (dy_mode_monitor_get_type ())

G_DECLARE_INTERFACE (DyModeMonitor, dy_mode_monitor, DY, MODE_MONITOR, GObject)

typedef struct _DyModeMonitorInfo DyModeMonitorInfo;

struct _DyModeMonitorInterface
{
    GTypeInterface g_iface;

    const DyModeMonitorInfo* (*get_info)    (DyModeMonitor *monitor);
    const char*              (*get_mode_id) (DyModeMonitor *monitor);
};

struct _DyModeMonitorInfo
{
    const char *mode_id;
    uint32_t    width;
    uint32_t    height;
};


const DyModeMonitorInfo *dy_mode_monitor_get_info    (DyModeMonitor *monitor);
const char              *dy_mode_monitor_get_mode_id (DyModeMonitor *monitor);

G_END_DECLS

#endif /* !DY_MODE_MONITOR_H */
