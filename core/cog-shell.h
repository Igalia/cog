/*
 * cog-shell.h
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-export.h"
#include "cog-request-handler.h"

G_BEGIN_DECLS

#define COG_VIEWPORT_DEFAULT GUINT_TO_POINTER(0)

typedef struct _CogViewport   CogViewport;
typedef struct _WebKitWebView WebKitWebView;

typedef gpointer CogViewportKey;

#define COG_TYPE_SHELL (cog_shell_get_type())

COG_API
G_DECLARE_DERIVABLE_TYPE(CogShell, cog_shell, COG, SHELL, GObject)

struct _CogShellClass {
    GObjectClass parent_class;

    /*< public >*/
    WebKitWebView* (*create_view) (CogShell*);
    void           (*startup)     (CogShell*);
    void           (*shutdown)    (CogShell*);
};

COG_API CogShell         *cog_shell_new(const char *name, gboolean automated);
COG_API const char       *cog_shell_get_name(CogShell *shell);
COG_API WebKitWebContext *cog_shell_get_web_context(CogShell *shell);
COG_API WebKitSettings   *cog_shell_get_web_settings(CogShell *shell);
COG_API WebKitWebView    *cog_shell_get_web_view(CogShell *shell);
COG_API GKeyFile         *cog_shell_get_config_file(CogShell *shell);
COG_API gdouble           cog_shell_get_device_scale_factor(CogShell *shell);
COG_API gboolean          cog_shell_is_automated(CogShell *shell);
COG_API void cog_shell_set_request_handler(CogShell *shell, const char *scheme, CogRequestHandler *handler);

COG_API void         cog_shell_startup(CogShell *shell);
COG_API void         cog_shell_shutdown(CogShell *shell);
COG_API CogViewport *cog_shell_get_viewport(CogShell *shell);
COG_API GHashTable  *cog_shell_get_viewports(CogShell *shell);

COG_API CogViewport *cog_shell_viewport_lookup(CogShell *, CogViewportKey);
COG_API CogViewport *cog_shell_viewport_new(CogShell *shell, CogViewportKey);
COG_API gboolean     cog_shell_viewport_remove(CogShell *shell, CogViewportKey);

G_END_DECLS
