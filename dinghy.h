/*
 * dinghy.h
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef DINGHY_H
#define DINGHY_H

#define DY_INSIDE_DINGHY__ 1

#include "dy-config.h"
#include "dy-webkit-utils.h"
#include "dy-request-handler.h"
#include "dy-directory-files-handler.h"
#include "dy-launcher.h"
#include "dy-platform.h"
#include "dy-utils.h"

#if DY_USE_MODE_MONITOR
# include "dy-mode-monitor.h"
# include "dy-sysfs-mode-monitor.h"
# if DY_USE_DRM_MODE_MONITOR
#  include "dy-drm-mode-monitor.h"
# endif /* DY_USE_DRM_MODE_MONITOR */
#endif /* DY_USE_MODE_MONITOR */

#undef DY_INSIDE_DINGHY__

#endif /* !DINGHY_H */
