/*
 * cog.h
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef COG_H
#define COG_H

#define COG_INSIDE_COG__ 1

#include "cog-config.h"
#include "cog-webkit-utils.h"
#include "cog-request-handler.h"
#include "cog-directory-files-handler.h"
#include "cog-launcher.h"
#include "cog-utils.h"

#if !COG_USE_WEBKITGTK
# include "cog-platform.h"
#endif /* !COG_USE_WEBKITGTK */

#if COG_USE_MODE_MONITOR
# include "cog-mode-monitor.h"
# include "cog-sysfs-mode-monitor.h"
# if COG_USE_DRM_MODE_MONITOR
#  include "cog-drm-mode-monitor.h"
# endif /* COG_USE_DRM_MODE_MONITOR */
#endif /* COG_USE_MODE_MONITOR */

#undef COG_INSIDE_COG__

#endif /* !COG_H */
