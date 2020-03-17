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
#include "cog-shell.h"

G_BEGIN_DECLS

#define COG_PLATFORM_EGL_ERROR  (cog_platform_egl_error_quark ())
GQuark cog_platform_egl_error_quark (void);

#define COG_PLATFORM_WPE_ERROR  (cog_platform_wpe_error_quark ())
GQuark cog_platform_wpe_error_quark (void);


typedef enum {
    COG_PLATFORM_WPE_ERROR_INIT,
} CogPlatformWpeError;


/* @FIXME: Eventually move this interface to GObject. */
typedef struct _CogPlatform CogPlatform;
typedef struct _WebKitInputMethodContext WebKitInputMethodContext;

CogPlatform              *cog_platform_new               (void);
void                      cog_platform_free              (CogPlatform   *platform);

gboolean                  cog_platform_try_load          (CogPlatform   *platform,
                                                          const gchar   *soname);

gboolean                  cog_platform_setup             (CogPlatform   *platform,
                                                          CogShell      *shell,
                                                          const char    *params,
                                                          GError       **error);

void                      cog_platform_teardown          (CogPlatform   *platform);

WebKitWebViewBackend     *cog_platform_get_view_backend  (CogPlatform   *platform,
                                                          WebKitWebView *related_view,
                                                          GError       **error);

void                      cog_platform_init_web_view     (CogPlatform   *platform,
                                                          WebKitWebView *view);

WebKitInputMethodContext *cog_platform_create_im_context (CogPlatform   *platform);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CogPlatform, cog_platform_free)

G_END_DECLS

#endif /* !COG_PLATFORM_H */
