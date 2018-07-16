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
#include "cog-webkit-utils.h"

G_BEGIN_DECLS

#define COG_TYPE_SHELL  (cog_shell_get_type ())

G_DECLARE_DERIVABLE_TYPE (CogShell, cog_shell, COG, SHELL, GObject)


struct _CogShellClass {
    GObjectClass parent_class;

    WebKitWebView* (*create_view) (CogShell*);
    void           (*startup)     (CogShell*);
    void           (*shutdown)    (CogShell*);
};


CogShell         *cog_shell_new                 (const char        *name);
const char       *cog_shell_get_name            (CogShell          *shell);
WebKitWebContext *cog_shell_get_web_context     (CogShell          *shell);
WebKitSettings   *cog_shell_get_web_settings    (CogShell          *shell);
WebKitWebView    *cog_shell_get_web_view        (CogShell          *shell);
const char       *cog_shell_get_home_uri        (CogShell          *shell);
void              cog_shell_set_home_uri        (CogShell          *shell,
                                                 const char        *uri);
void              cog_shell_set_request_handler (CogShell          *shell,
                                                 const char        *scheme,
                                                 CogRequestHandler *handler);

void              cog_shell_startup             (CogShell          *shell);
void              cog_shell_shutdown            (CogShell          *shell);

G_END_DECLS
