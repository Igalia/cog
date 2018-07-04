/*
 * cog-platform.h
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef COG_PLATFORM_H
#define COG_PLATFORM_H

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include <glib.h>
#include "cog-launcher.h"

G_BEGIN_DECLS

/* @FIXME: Eventually move this interface to GObject. */
typedef struct _CogPlatform CogPlatform;

CogPlatform          *cog_platform_new              (void);
void                  cog_platform_free             (CogPlatform   *platform);

gboolean              cog_platform_try_load         (CogPlatform   *platform,
                                                     const gchar   *soname);

gboolean              cog_platform_setup            (CogPlatform   *platform,
                                                     CogLauncher   *launcher,
                                                     const char    *params,
                                                     GError       **error);

void                  cog_platform_teardown         (CogPlatform   *platform);

WebKitWebViewBackend *cog_platform_get_view_backend (CogPlatform   *platform,
                                                     WebKitWebView *related_view,
                                                     GError       **error);

void
cog_platform_view_show (CogPlatform         *platform,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data);

gboolean
cog_platform_view_show_finish (CogPlatform  *platform,
                               GAsyncResult *result,
                               GError      **error);

void
cog_platform_view_hide (CogPlatform         *platform,
                        GAsyncReadyCallback callback,
                        gpointer            user_data);

gboolean
cog_platform_view_hide_finish (CogPlatform  *platform,
                               GAsyncResult *result,
                               GError      **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CogPlatform, cog_platform_free)

G_END_DECLS

#endif /* !COG_PLATFORM_H */
