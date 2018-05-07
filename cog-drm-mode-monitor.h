/*
 * cog-drm-mode-monitor.h
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef COG_DRM_MODE_MONITOR_H
#define COG_DRM_MODE_MONITOR_H

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include <gio/gio.h>

G_BEGIN_DECLS

#define COG_TYPE_DRM_MODE_MONITOR  (cog_drm_mode_monitor_get_type ())
#define COG_DRM_MODE_MONITOR_ERROR (cog_drm_mode_monitor_error_quark ())

G_DECLARE_FINAL_TYPE (CogDrmModeMonitor, cog_drm_mode_monitor, COG, DRM_MODE_MONITOR, GObject)

typedef enum {
    COG_DRM_MODE_MONITOR_ERROR_UNAVAILABLE,
    COG_DRM_MODE_MONITOR_ERROR_UDEV,
} CogDrmModeMonitorError;


struct _CogDrmModeMonitorClass
{
    GObjectClass parent_class;
};


CogDrmModeMonitor *cog_drm_mode_monitor_new             (GFile             *file,
                                                         GError           **error);
const char        *cog_drm_mode_monitor_get_device_path (CogDrmModeMonitor *monitor);

G_END_DECLS

#endif /* !COG_DRM_MODE_MONITOR_H */
