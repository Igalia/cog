/*
 * cog-gtk-utils.c
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-gtk-utils.h"
#include "cog-launcher.h"
#include <gtk/gtk.h>


GtkWidget*
cog_gtk_create_window (CogLauncher *launcher)
{
    GtkWidget *button;

    g_return_val_if_fail (launcher, NULL);

    GtkWidget *web_view = GTK_WIDGET (cog_launcher_get_web_view (launcher));
    g_return_val_if_fail (web_view, NULL);

    gtk_window_set_default_icon_name ("applications-internet");

    GtkWidget *header = gtk_header_bar_new ();  // Floating.
    gtk_header_bar_set_show_close_button (GTK_HEADER_BAR (header), TRUE);
    gtk_header_bar_set_title (GTK_HEADER_BAR (header), "Cog");
    g_object_bind_property (web_view, "uri", header, "subtitle", G_BINDING_DEFAULT);

    /* HBox used to show Previous/Next buttons linked */
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class (gtk_widget_get_style_context (box), "raised");
    gtk_style_context_add_class (gtk_widget_get_style_context (box), "linked");
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header), box);
    gtk_widget_show (box);

    /* Back/Previous button */
    button = gtk_button_new_from_icon_name ("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text (button, "Go back to the previous page");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (button), "app.previous");
    gtk_container_add (GTK_CONTAINER (box), button);
    gtk_widget_show (button);

    /* Next/Forward button */
    button = gtk_button_new_from_icon_name ("go-next-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text (button, "Go forward to the next page");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (button), "app.next");
    gtk_container_add (GTK_CONTAINER (box), button);
    gtk_widget_show (button);

    /* Refresh button */
    button = gtk_button_new_from_icon_name ("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text (button, "Reload the current page");
    gtk_actionable_set_detailed_action_name (GTK_ACTIONABLE (button), "app.reload");
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header), button);
    gtk_widget_show (button);

    GtkWidget *window = gtk_application_window_new (GTK_APPLICATION (launcher));  // Floating.
    gtk_window_set_titlebar (GTK_WINDOW (window), header);  // Takes ownwership of header.
    gtk_window_set_default_size (GTK_WINDOW (window), 800, 700);
    gtk_widget_set_size_request (window, 300, 200);
    gtk_container_add (GTK_CONTAINER (window), web_view);

    gtk_widget_show_all (window);
    return window;
}


void
cog_gtk_present_window (CogLauncher *launcher)
{
    g_return_if_fail (COG_IS_LAUNCHER (launcher));

    GtkWidget *web_view = GTK_WIDGET (cog_launcher_get_web_view (launcher));
    g_assert_nonnull (web_view);

    GtkWidget *toplevel = gtk_widget_get_toplevel (web_view);
    if (gtk_widget_is_toplevel (toplevel) && GTK_IS_WINDOW (toplevel))
        gtk_window_present (GTK_WINDOW (toplevel));
}
