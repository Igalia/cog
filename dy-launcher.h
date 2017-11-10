/*
 * dy-launcher.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef DY_LAUNCHER_H
#define DY_LAUNCHER_H

#include "dy-webkit-utils.h"

G_BEGIN_DECLS

#if DY_WEBKIT_GTK
# define DY_LAUNCHER_BASE_TYPE  GTK_TYPE_APPLICATION
  typedef GtkApplication      DyLauncherBase;
  typedef GtkApplicationClass DyLauncherBaseClass;
#else
# define DY_LAUNCHER_BASE_TYPE  G_TYPE_APPLICATION
  typedef GApplication        DyLauncherBase;
  typedef GApplicationClass   DyLauncherBaseClass;
#endif

G_DEFINE_AUTOPTR_CLEANUP_FUNC (DyLauncherBase, g_object_unref)

#define DY_TYPE_LAUNCHER (dy_launcher_get_type ())

G_DECLARE_FINAL_TYPE (DyLauncher, dy_launcher, DY, LAUNCHER, DyLauncherBase)

struct _DyLauncherClass
{
    DyLauncherBaseClass parent_class;
};

DyLauncher       *dy_launcher_get_default     (void);
WebKitWebView    *dy_launcher_get_web_view    (DyLauncher *launcher);
WebKitWebContext *dy_launcher_get_web_context (DyLauncher *launcher);

G_END_DECLS

#endif /* !DY_LAUNCHER_H */
