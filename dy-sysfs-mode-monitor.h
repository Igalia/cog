/*
 * dy-sysfs-mode-monitor.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef DY_SYSFS_MODE_MONITOR_H
#define DY_SYSFS_MODE_MONITOR_H

#if !(defined(DY_INSIDE_DINGHY__) && DY_INSIDE_DINGHY__)
# error "Do not include this header directly, use <dinghy.h> instead"
#endif

#include <gio/gio.h>

G_BEGIN_DECLS

#define DY_TYPE_SYSFS_MODE_MONITOR  (dy_sysfs_mode_monitor_get_type ())

G_DECLARE_FINAL_TYPE (DySysfsModeMonitor, dy_sysfs_mode_monitor, DY, SYSFS_MODE_MONITOR, GObject);

struct _DySysfsModeMonitorClass
{
    GObjectClass parent_class;
};


DySysfsModeMonitor *dy_sysfs_mode_monitor_new      (GFile              *file,
                                                    GError            **error);
const char         *dy_sysfs_mode_monitor_get_path (DySysfsModeMonitor *monitor);

G_END_DECLS

#endif /* !DY_SYSFS_MODE_MONITOR_H */
