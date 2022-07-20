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

#include "cog-shell.h"
#include <glib-object.h>

G_BEGIN_DECLS

#define COG_PLATFORM_ERROR (cog_platform_error_quark())

typedef enum {
    COG_PLATFORM_ERROR_NO_MODULE,
} CogPlatformError;

#define COG_PLATFORM_EGL_ERROR  (cog_platform_egl_error_quark ())
GQuark cog_platform_egl_error_quark (void);

#define COG_PLATFORM_WPE_ERROR  (cog_platform_wpe_error_quark ())
GQuark cog_platform_wpe_error_quark (void);

typedef enum {
    COG_PLATFORM_WPE_ERROR_INIT,
} CogPlatformWpeError;

#define COG_TYPE_PLATFORM (cog_platform_get_type())

G_DECLARE_DERIVABLE_TYPE(CogPlatform, cog_platform, COG, PLATFORM, GObject)

struct _CogPlatformClass {
    GObjectClass parent_class;

    /*< public >*/
    gboolean (*is_supported)(void);
    gboolean (*setup)(CogPlatform *, CogShell *shell, const char *params, GError **);
    WebKitWebViewBackend *(*get_view_backend)(CogPlatform *, WebKitWebView *related_view, GError **);
    void (*init_web_view)(CogPlatform *, WebKitWebView *);
    WebKitInputMethodContext *(*create_im_context)(CogPlatform *);
};

void cog_platform_set_default(CogPlatform *);
CogPlatform *cog_platform_get_default(void);
CogPlatform *cog_platform_new(const char *name, GError **);

gboolean cog_platform_setup(CogPlatform *platform, CogShell *shell, const char *params, GError **error);

WebKitWebViewBackend     *cog_platform_get_view_backend  (CogPlatform   *platform,
                                                          WebKitWebView *related_view,
                                                          GError       **error);

void                      cog_platform_init_web_view     (CogPlatform   *platform,
                                                          WebKitWebView *view);

WebKitInputMethodContext *cog_platform_create_im_context (CogPlatform   *platform);

G_END_DECLS

#endif /* !COG_PLATFORM_H */
