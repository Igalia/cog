/*
 * cog-launcher.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef COG_LAUNCHER_H
#define COG_LAUNCHER_H

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include "cog-config.h"
#include "cog-shell.h"
#include "cog-webkit-utils.h"

G_BEGIN_DECLS

typedef struct _CogRequestHandler CogRequestHandler;


#if COG_USE_WEBKITGTK
# define COG_LAUNCHER_BASE_TYPE GTK_TYPE_APPLICATION
  typedef GtkApplication       CogLauncherBase;
  typedef GtkApplicationClass  CogLauncherBaseClass;
#else
# define COG_LAUNCHER_BASE_TYPE G_TYPE_APPLICATION
  typedef GApplication         CogLauncherBase;
  typedef GApplicationClass    CogLauncherBaseClass;
#endif

G_DEFINE_AUTOPTR_CLEANUP_FUNC (CogLauncherBase, g_object_unref)

#define COG_TYPE_LAUNCHER (cog_launcher_get_type ())

G_DECLARE_FINAL_TYPE (CogLauncher, cog_launcher, COG, LAUNCHER, CogLauncherBase)

struct _CogLauncherClass
{
    CogLauncherBaseClass parent_class;
};

CogLauncher *cog_launcher_get_default                  (void);
CogShell    *cog_launcher_get_shell                    (CogLauncher *launcher);

void  cog_launcher_add_web_settings_option_entries     (CogLauncher *launcher);
void  cog_launcher_add_web_cookies_option_entries      (CogLauncher *launcher);
void  cog_launcher_add_web_permissions_option_entries  (CogLauncher *launcher);

G_END_DECLS

#endif /* !COG_LAUNCHER_H */
