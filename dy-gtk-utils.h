/*
 * dy-gtk-utils.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef DY_GTK_UTILS_H
#define DY_GTK_UTILS_H

#if !(defined(DY_INSIDE_DINGHY__) && DY_INSIDE_DINGHY__)
# error "Do not include this header directly, use <dinghy.h> instead"
#endif

#include <glib.h>

typedef struct _GtkWidget  GtkWidget;
typedef struct _DyLauncher DyLauncher;

G_BEGIN_DECLS

GtkWidget* dy_gtk_create_window  (DyLauncher *launcher);
void       dy_gtk_present_window (DyLauncher *launcher);

G_END_DECLS

#endif /* !DY_GTK_UTILS_H */
