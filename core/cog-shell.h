/*
 * cog-shell.h
 * Copyright (C) 2018-2019 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-view.h"

G_BEGIN_DECLS

#define COG_TYPE_SHELL  (cog_shell_get_type ())

G_DECLARE_DERIVABLE_TYPE (CogShell, cog_shell, COG, SHELL, GObject)

struct _CogShellClass {
    GObjectClass parent_class;

    gboolean (*is_supported)   (void);
    GType    (*get_view_class) (void);
    WebKitWebViewBackend* (*cog_shell_new_view_backend) (CogShell *shell);
};

CogShell*   cog_shell_new             (const char *name);

CogShell*   cog_shell_new_from_module (const char *name,
                                       const char *module_name);

const char* cog_shell_get_name        (CogShell   *shell);

CogView*    cog_shell_create_view     (CogShell   *shell,
                                       const char *name,
                                       const char *prop_1,
                                       ...);

void        cog_shell_add_view        (CogShell   *shell,
                                       CogView    *view);

CogView*    cog_shell_get_view        (CogShell   *shell,
                                       const char *name);

GList*      cog_shell_get_views       (CogShell   *shell);

CogView*    cog_shell_get_active_view (CogShell   *shell);

void        cog_shell_set_active_view (CogShell   *shell,
                                       CogView    *view);

WebKitWebViewBackend* cog_shell_new_view_backend(CogShell *shell);
G_END_DECLS
