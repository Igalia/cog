/*
 * dy-gtk-utils.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef DY_GTK_UTILS_H
#define DY_GTK_UTILS_H

#include <gtk/gtk.h>
#include "dinghy.h"

G_BEGIN_DECLS

GtkWidget* dy_gtk_create_window  (DyLauncher *launcher);
void       dy_gtk_present_window (DyLauncher *launcher);

G_END_DECLS

#endif /* !DY_GTK_UTILS_H */
