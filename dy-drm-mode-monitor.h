/*
 * dy-drm-mode-monitor.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef DY_DRM_MODE_MONITOR_H
#define DY_DRM_MODE_MONITOR_H

#if !(defined(DY_INSIDE_DINGHY__) && DY_INSIDE_DINGHY__)
# error "Do not include this header directly, use <dinghy.h> instead"
#endif

#include <gio/gio.h>

G_BEGIN_DECLS

#define DY_TYPE_DRM_MODE_MONITOR  (dy_drm_mode_monitor_get_type ())
#define DY_DRM_MODE_MONITOR_ERROR (dy_drm_mode_monitor_error_quark ())

G_DECLARE_FINAL_TYPE (DyDrmModeMonitor, dy_drm_mode_monitor, DY, DRM_MODE_MONITOR, GObject)

typedef enum {
    DY_DRM_MODE_MONITOR_ERROR_UNAVAILABLE,
    DY_DRM_MODE_MONITOR_ERROR_UDEV,
} DyDrmModeMonitorError;


struct _DyDrmModeMonitorClass
{
    GObjectClass parent_class;
};


DyDrmModeMonitor *dy_drm_mode_monitor_new             (GFile            *file,
                                                       GError          **error);
const char       *dy_drm_mode_monitor_get_device_path (DyDrmModeMonitor *monitor);

G_END_DECLS

#endif /* !DY_DRM_MODE_MONITOR_H */
