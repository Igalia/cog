/*
 * cog-sysfs-mode-monitor.h
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef COG_SYSFS_MODE_MONITOR_H
#define COG_SYSFS_MODE_MONITOR_H

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include <gio/gio.h>

G_BEGIN_DECLS

#define COG_TYPE_SYSFS_MODE_MONITOR  (cog_sysfs_mode_monitor_get_type ())

G_DECLARE_FINAL_TYPE (CogSysfsModeMonitor, cog_sysfs_mode_monitor, COG, SYSFS_MODE_MONITOR, GObject);

struct _CogSysfsModeMonitorClass
{
    GObjectClass parent_class;
};


CogSysfsModeMonitor *cog_sysfs_mode_monitor_new      (GFile               *file,
                                                      GError             **error);
const char          *cog_sysfs_mode_monitor_get_path (CogSysfsModeMonitor *monitor);

G_END_DECLS

#endif /* !COG_SYSFS_MODE_MONITOR_H */
