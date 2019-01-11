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

typedef struct _WebKitWebViewBackend WebKitWebViewBackend;


#define COG_PLATFORM_EGL_ERROR  (cog_platform_egl_error_quark ())
GQuark cog_platform_egl_error_quark (void);

#define COG_PLATFORM_ERROR  (cog_platform_error_quark ())
GQuark cog_platform_error_quark (void);

typedef enum {
    COG_PLATFORM_ERROR_LOAD,
    COG_PLATFORM_ERROR_SETUP,
} CogPlatformError;


#define COG_TYPE_PLATFORM_VIEW (cog_platform_view_get_type ())

G_DECLARE_INTERFACE (CogPlatformView, cog_platform_view, COG, PLATFORM_VIEW, GObject)


struct _CogPlatformViewInterface
{
    GTypeInterface g_iface;

    WebKitWebViewBackend* (*get_backend) (CogPlatformView *platform_view);

    void                  (*__padding0)  (void);
    void                  (*__padding1)  (void);
    void                  (*__padding2)  (void);
    void                  (*__padding3)  (void);
    void                  (*__padding4)  (void);
};


static inline WebKitWebViewBackend*
cog_platform_view_get_backend (CogPlatformView *platform_view)
{
    g_return_val_if_fail (COG_IS_PLATFORM_VIEW (platform_view), NULL);
    return COG_PLATFORM_VIEW_GET_IFACE (platform_view)->get_backend (platform_view);
}


#define COG_TYPE_PLATFORM  (cog_platform_get_type ())

G_DECLARE_INTERFACE (CogPlatform, cog_platform, COG, PLATFORM, GObject)


struct _CogPlatformInterface
{
    GTypeInterface    g_iface;

    gboolean         (*setup)       (CogPlatform    *platform,
                                     CogShell       *shell,
                                     const char     *params,
                                     GError        **error);

    void             (*teardown)    (CogPlatform    *platform);

    CogPlatformView* (*create_view) (CogPlatform    *platform,
                                     WebKitWebView  *related_view,
                                     GError        **error);

    void             (*__padding0)   (void);
    void             (*__padding1)   (void);
    void             (*__padding2)   (void);
    void             (*__padding3)   (void);
    void             (*__padding4)   (void);
};


CogPlatform* cog_platform_load (const gchar  *soname,
                                GError      **error);

static inline gboolean
cog_platform_setup (CogPlatform  *platform,
                    CogShell     *shell,
                    const char   *params,
                    GError      **error)
{
    g_return_val_if_fail (COG_IS_PLATFORM (platform), FALSE);
    COG_PLATFORM_GET_IFACE (platform)->setup (platform,
                                              shell,
                                              params,
                                              error);
}

static inline void
cog_platform_teardown (CogPlatform *platform)
{
    g_return_if_fail (COG_IS_PLATFORM (platform));
    COG_PLATFORM_GET_IFACE (platform)->teardown (platform);
}

static inline CogPlatformView*
cog_platform_create_view (CogPlatform    *platform,
                          WebKitWebView  *related_view,
                          GError        **error)
{
    g_return_val_if_fail (COG_IS_PLATFORM (platform), NULL);
    CogPlatformInterface *iface = COG_PLATFORM_GET_IFACE (platform);
    g_assert_nonnull (iface->create_view);

    return (*iface->create_view) (platform, related_view, error);
}


#define COG_TYPE_SIMPLE_PLATFORM_VIEW  (cog_simple_platform_view_get_type ())

G_DECLARE_FINAL_TYPE (CogSimplePlatformView,
                      cog_simple_platform_view,
                      COG, SIMPLE_PLATFORM_VIEW,
                      GObject)

struct _CogSimplePlatformViewClass {
    GObjectClass parent_class;
};

CogPlatformView* cog_simple_platform_view_new (WebKitWebViewBackend *backend);

G_END_DECLS

#endif /* !COG_PLATFORM_H */
