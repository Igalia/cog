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

#define COG_SHELL_DEFAULT_VIEWPORT_INDEX 0

typedef struct _CogView CogView;

#define COG_TYPE_SHELL (cog_shell_get_type())

COG_API
G_DECLARE_DERIVABLE_TYPE(CogShell, cog_shell, COG, SHELL, GObject)

struct _CogShellClass {
    GObjectClass parent_class;

    /*< public >*/
    void (*startup)(CogShell *self);
    void (*shutdown)(CogShell *self);
};

COG_API CogShell         *cog_shell_new(const char *name, gboolean automated);
COG_API const char       *cog_shell_get_name(CogShell *shell);
COG_API WebKitWebContext *cog_shell_get_web_context(CogShell *shell);
COG_API WebKitSettings   *cog_shell_get_web_settings(CogShell *shell);
COG_API GKeyFile         *cog_shell_get_config_file(CogShell *shell);
COG_API gdouble           cog_shell_get_device_scale_factor(CogShell *shell);
COG_API gboolean          cog_shell_is_automated(CogShell *shell);
COG_API void cog_shell_set_request_handler(CogShell *shell, const char *scheme, CogRequestHandler *handler);

COG_API void cog_shell_startup(CogShell *shell);
COG_API void cog_shell_shutdown(CogShell *shell);

G_END_DECLS
