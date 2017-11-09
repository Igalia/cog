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

#define DY_TYPE_LAUNCHER (dy_launcher_get_type ())

#if DY_WEBKIT_GTK
G_DECLARE_FINAL_TYPE (DyLauncher, dy_launcher, DY, LAUNCHER, GtkApplication)
#else
G_DECLARE_FINAL_TYPE (DyLauncher, dy_launcher, DY, LAUNCHER, GApplication)
#endif

struct _DyLauncherClass
{
#if DY_WEBKIT_GTK
    GtkApplicationClass parent_class;
#else
    GApplicationClass parent_class;
#endif
};

DyLauncher       *dy_launcher_get_default     (void);
WebKitWebContext *dy_launcher_get_web_context (DyLauncher *launcher);

G_END_DECLS

#endif /* !DY_LAUNCHER_H */
