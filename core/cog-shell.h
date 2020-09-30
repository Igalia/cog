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

#define COG_SHELL_ERROR (cog_shell_error_quark ())

typedef enum {
    COG_SHELL_ERROR_MODULE_NOT_FOUND,
} CogShellError;

#define COG_TYPE_SHELL  (cog_shell_get_type ())

G_DECLARE_DERIVABLE_TYPE (CogShell, cog_shell, COG, SHELL, GObject)

struct _CogShellClass {
    GObjectClass parent_class;

    gboolean (*is_supported)   (void);
    GType    (*get_view_class) (void);

    void     (*padding_0)      (void);
    void     (*padding_1)      (void);
    void     (*padding_2)      (void);
    void     (*padding_3)      (void);
    void     (*padding_4)      (void);
};

GQuark      cog_shell_error_quark      (void);

CogShell*   cog_shell_new              (GError    **error,
                                        const char *name,
                                        const char *prop_1,
                                        ...);

CogShell*   cog_shell_new_from_module  (GError    **error,
                                        const char *name,
                                        const char *module_name,
                                        const char *prop_1,
                                        ...);

const char* cog_shell_get_name         (CogShell   *shell);

CogView*    cog_shell_add_view         (CogShell   *shell,
                                        const char *name,
                                        const char *prop_1,
                                        ...);

void        cog_shell_remove_view      (CogShell   *shell,
                                        const char *name);

CogView*    cog_shell_get_view         (CogShell   *shell,
                                        const char *name);

GList*      cog_shell_get_views        (CogShell   *shell);

CogView*    cog_shell_get_focused_view (CogShell   *shell);

G_END_DECLS
