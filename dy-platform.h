/*
 * dy-platform.h
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef DY_PLATFORM_H
#define DY_PLATFORM_H

#if !(defined(DY_INSIDE_DINGHY__) && DY_INSIDE_DINGHY__)
# error "Do not include this header directly, use <dinghy.h> instead"
#endif

#include <glib.h>
#include "dy-config.h"
#include "dy-launcher.h"

G_BEGIN_DECLS

/* @FIXME: Eventually move this interface to GObject. */
typedef struct _DyPlatform DyPlatform;

DyPlatform*           dy_platform_new              (void);
void                  dy_platform_free             (DyPlatform *platform);

gboolean              dy_platform_try_load         (DyPlatform  *platform,
                                                    const gchar *soname);

gboolean              dy_platform_setup            (DyPlatform  *platform,
                                                    DyLauncher  *launcher,
                                                    const char  *params,
                                                    GError     **error);

void                  dy_platform_teardown         (DyPlatform *platform);

WebKitWebViewBackend* dy_platform_get_view_backend (DyPlatform     *platform,
                                                    WebKitWebView  *related_view,
                                                    GError        **error);

G_END_DECLS

#endif /* !DY_PLATFORM_H */
