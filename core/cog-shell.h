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

#define COG_VIEW_STACK_DEFAULT GUINT_TO_POINTER(0)

G_BEGIN_DECLS

typedef gpointer CogViewStackKey;

typedef struct _CogViewStack  CogViewStack;
typedef struct _WebKitWebView WebKitWebView;

#define COG_TYPE_SHELL  (cog_shell_get_type ())

G_DECLARE_DERIVABLE_TYPE (CogShell, cog_shell, COG, SHELL, GObject)


struct _CogShellClass {
    GObjectClass parent_class;

    /*< public >*/
    WebKitWebView *(*create_view)(CogShell *);
    void (*startup)(CogShell *);
    void (*shutdown)(CogShell *);
};

CogShell         *cog_shell_new(const char *name, gboolean automated);
const char       *cog_shell_get_name(CogShell *);
WebKitWebContext *cog_shell_get_web_context(CogShell *);
WebKitSettings   *cog_shell_get_web_settings(CogShell *);
WebKitWebView    *cog_shell_get_web_view_default(CogShell *);
WebKitWebView    *cog_shell_get_web_view(CogShell *, CogViewStackKey);
GHashTable       *cog_shell_get_view_stacks(CogShell *);
CogViewStack     *cog_shell_view_stack_lookup(CogShell *, CogViewStackKey);
CogViewStack     *cog_shell_view_stack_new(CogShell *, CogViewStackKey);
bool              cog_shell_view_stack_remove(CogShell *, CogViewStackKey);
GKeyFile         *cog_shell_get_config_file(CogShell *);
gdouble           cog_shell_get_device_scale_factor(CogShell *);
gboolean          cog_shell_is_automated(CogShell *);
void              cog_shell_set_request_handler(CogShell *, const char *scheme, CogRequestHandler *);

void cog_shell_startup(CogShell *);
void cog_shell_shutdown(CogShell *);

G_END_DECLS
