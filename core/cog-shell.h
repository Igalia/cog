/*
 * cog-shell.h
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-request-handler.h"

G_BEGIN_DECLS

typedef struct _CogViewStack  CogViewStack;
typedef struct _WebKitWebView WebKitWebView;

#define COG_TYPE_SHELL  (cog_shell_get_type ())

G_DECLARE_DERIVABLE_TYPE (CogShell, cog_shell, COG, SHELL, GObject)


struct _CogShellClass {
    GObjectClass parent_class;

    /*< public >*/
    WebKitWebView* (*create_view) (CogShell*);
    void           (*startup)     (CogShell*);
    void           (*shutdown)    (CogShell*);
};

CogShell *        cog_shell_new(const char *name, gboolean automated);
const char *      cog_shell_get_name(CogShell *shell);
WebKitWebContext *cog_shell_get_web_context(CogShell *shell);
WebKitSettings   *cog_shell_get_web_settings        (CogShell          *shell);
WebKitWebView    *cog_shell_get_web_view            (CogShell          *shell);
CogViewStack     *cog_shell_get_view_stack(CogShell *shell);
GKeyFile         *cog_shell_get_config_file         (CogShell          *shell);
gdouble           cog_shell_get_device_scale_factor (CogShell          *shell);
gboolean          cog_shell_is_automated(CogShell *shell);
void              cog_shell_set_request_handler(CogShell *shell, const char *scheme, CogRequestHandler *handler);

void              cog_shell_startup                 (CogShell          *shell);
void              cog_shell_shutdown                (CogShell          *shell);

G_END_DECLS
