/*
 * dy-launcher.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef DY_LAUNCHER_H
#define DY_LAUNCHER_H

#if !(defined(DY_INSIDE_DINGHY__) && DY_INSIDE_DINGHY__)
# error "Do not include this header directly, use <dinghy.h> instead"
#endif

#include "dy-config.h"
#include "dy-webkit-utils.h"

G_BEGIN_DECLS

#if DY_USE_WEBKITGTK
# define DY_LAUNCHER_BASE_TYPE GTK_TYPE_APPLICATION
  typedef GtkApplication       DyLauncherBase;
  typedef GtkApplicationClass  DyLauncherBaseClass;
#else
# define DY_LAUNCHER_BASE_TYPE G_TYPE_APPLICATION
  typedef GApplication         DyLauncherBase;
  typedef GApplicationClass    DyLauncherBaseClass;
#endif

G_DEFINE_AUTOPTR_CLEANUP_FUNC (DyLauncherBase, g_object_unref)

#define DY_TYPE_LAUNCHER (dy_launcher_get_type ())

G_DECLARE_FINAL_TYPE (DyLauncher, dy_launcher, DY, LAUNCHER, DyLauncherBase)

struct _DyLauncherClass
{
    DyLauncherBaseClass parent_class;
};

DyLauncher       *dy_launcher_get_default     (void);
WebKitWebView    *dy_launcher_get_web_view    (DyLauncher   *launcher);
WebKitWebContext *dy_launcher_get_web_context (DyLauncher   *launcher);
const char       *dy_launcher_get_home_uri    (DyLauncher   *launcher);
void              dy_launcher_set_home_uri    (DyLauncher   *launcher,
                                               const char   *home_uri);

G_END_DECLS

#endif /* !DY_LAUNCHER_H */
