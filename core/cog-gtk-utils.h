/*
 * cog-gtk-utils.h
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef COG_GTK_UTILS_H
#define COG_GTK_UTILS_H

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include <glib.h>

typedef struct _GtkWidget   GtkWidget;
typedef struct _CogLauncher CogLauncher;

G_BEGIN_DECLS

GtkWidget* cog_gtk_create_window  (CogLauncher *launcher);
void       cog_gtk_present_window (CogLauncher *launcher);

G_END_DECLS

#endif /* !COG_GTK_UTILS_H */
