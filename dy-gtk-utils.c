/*
 * dy-gtk-utils.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "dy-gtk-utils.h"


GtkWidget*
dy_gtk_create_window (DyLauncher *launcher)
{
    g_return_val_if_fail (launcher, NULL);

    GtkWidget *web_view = GTK_WIDGET (dy_launcher_get_web_view (launcher));
    g_return_val_if_fail (web_view, NULL);

    GtkWidget *header = gtk_header_bar_new ();  // Floating.
    gtk_header_bar_set_show_close_button (GTK_HEADER_BAR (header), TRUE);
    gtk_header_bar_set_title (GTK_HEADER_BAR (header), "Dinghy");
    g_object_bind_property (web_view, "uri", header, "subtitle", G_BINDING_DEFAULT);

    GtkWidget *window = gtk_application_window_new (GTK_APPLICATION (launcher));  // Floating.
    gtk_window_set_titlebar (GTK_WINDOW (window), header);  // Takes ownwership of header.
    gtk_widget_set_size_request (window, 400, 200);
    gtk_container_add (GTK_CONTAINER (window), web_view);

    gtk_widget_show_all (window);
    return window;
}


void
dy_gtk_present_window (DyLauncher *launcher)
{
    g_return_if_fail (launcher);

    GtkWidget *web_view = GTK_WIDGET (dy_launcher_get_web_view (launcher));
    g_return_if_fail (web_view);

    GtkWidget *toplevel = gtk_widget_get_toplevel (web_view);
    if (gtk_widget_is_toplevel (toplevel) && GTK_IS_WINDOW (toplevel))
        gtk_window_present (GTK_WINDOW (toplevel));
}
