/*
 * dy-gtk-utils.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "dy-gtk-utils.h"


static void
add_header_button (GtkWidget  *header,
                   const char *icon_name,
                   const char *action)
{
    g_return_if_fail (header);
    g_return_if_fail (icon_name);
    g_return_if_fail (action);

    GtkWidget *button = gtk_button_new_from_icon_name (icon_name, GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header), button);
    gtk_actionable_set_action_name (GTK_ACTIONABLE (button), action);
    gtk_widget_show (button);
}


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

    add_header_button (header, "go-previous", "app.previous");
    add_header_button (header, "go-next", "app.next");

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
