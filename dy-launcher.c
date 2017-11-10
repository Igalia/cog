/*
 * dy-launcher.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "dy-launcher.h"
#include "dy-webkit-utils.h"

#if DY_WEBKIT_GTK
# include "dy-gtk-utils.h"
#endif


struct _DyLauncher {
    DyLauncherBase    parent;
    WebKitWebContext *web_context;
    WebKitWebView    *web_view;
};

G_DEFINE_TYPE (DyLauncher, dy_launcher, DY_LAUNCHER_BASE_TYPE)


enum {
    CREATE_WEB_VIEW,
    LAST_SIGNAL,
};

static int s_signals[LAST_SIGNAL] = { 0, };


enum {
    PROP_0,
    PROP_WEB_CONTEXT,
    PROP_WEB_VIEW,
    N_PROPERTIES
};

static GParamSpec *s_properties[N_PROPERTIES] = { NULL, };


enum {
    ACTION_QUIT,
    ACTION_PREVIOUS,
    ACTION_NEXT,
    ACTION_RELOAD,
    N_ACTIONS
};

static GSimpleAction *s_actions[N_ACTIONS] = { NULL, };


static void
on_action_quit (DyLauncher *launcher)
{
    g_application_quit (G_APPLICATION (launcher));
}

static void
on_action_prev (DyLauncher *launcher)
{
    webkit_web_view_go_back (dy_launcher_get_web_view (launcher));
}

static void
on_action_next (DyLauncher *launcher)
{
    webkit_web_view_go_forward (dy_launcher_get_web_view (launcher));
}

static void
on_action_reload (DyLauncher *launcher)
{
    webkit_web_view_reload (dy_launcher_get_web_view (launcher));
}

static void
on_action_activate (GSimpleAction *action,
                    GVariant      *parameter,
                    void          *user_data)
{
    g_assert_nonnull (action);
    g_assert_nonnull (user_data);
    void (*callback)(DyLauncher*) = user_data;
    (*callback) (dy_launcher_get_default ());
}

static GSimpleAction*
make_action (const char *name, void (*callback) (DyLauncher*))
{
    g_return_val_if_fail (name, NULL);
    g_return_val_if_fail (callback, NULL);

    GSimpleAction *action = g_simple_action_new (name, NULL);
    g_signal_connect (action, "activate", G_CALLBACK (on_action_activate), callback);
    return action;
}


static void
dy_launcher_create_web_context (DyLauncher *launcher)
{
    g_autofree char *data_dir = g_build_filename (g_get_user_data_dir (),
                                                  g_get_prgname (),
                                                  NULL);
    g_autofree char *cache_dir = g_build_filename (g_get_user_cache_dir (),
                                                   g_get_prgname (),
                                                   NULL);

    g_autoptr(WebKitWebsiteDataManager) manager =
        webkit_website_data_manager_new ("base-data-directory", data_dir,
                                         "base-cache-directory", cache_dir,
                                         NULL);

    launcher->web_context = webkit_web_context_new_with_website_data_manager (manager);
}


static void
dy_launcher_open (GApplication *application,
                  GFile       **files,
                  int           n_files,
                  const char   *hint)
{
    g_printerr ("%s: %i files\n", __func__, n_files);
}

static void
dy_launcher_startup (GApplication *application)
{
    G_APPLICATION_CLASS (dy_launcher_parent_class)->startup (application);

    DyLauncher *launcher = DY_LAUNCHER (application);
    dy_launcher_create_web_context (launcher);
}

static void
dy_launcher_shutdown (GApplication *application)
{
    G_APPLICATION_CLASS (dy_launcher_parent_class)->shutdown (application);

    DyLauncher *launcher = DY_LAUNCHER (application);
}

static void
dy_launcher_activate (GApplication *application)
{
    /*
     * Give user code the chance of creating a customized web view.
     */
    DyLauncher *launcher = DY_LAUNCHER (application);

    /*
     * We support only a single main web view, so bail out here if one
     * has been already created (there might be related ones, though).
     */
    if (launcher->web_view) {
#if DY_WEBKIT_GTK
        dy_gtk_present_window (launcher);
#endif
        return;
    }

    g_signal_emit (launcher,
                   s_signals[CREATE_WEB_VIEW],
                   0,
                   &launcher->web_view);

    /*
     * Create the web view ourselves if the signal handler did not.
     */
    if (!launcher->web_view) {
        launcher->web_view = WEBKIT_WEB_VIEW (webkit_web_view_new_with_context (dy_launcher_get_web_context (launcher)));
#if DY_WEBKIT_GTK
        GtkWidget *window = dy_gtk_create_window (launcher);
        g_object_ref_sink (window);  // Keep the window object alive.
#endif
    }

    g_object_ref_sink (launcher->web_view);
    g_return_if_fail (webkit_web_view_get_context (launcher->web_view) == dy_launcher_get_web_context (launcher));
}


static void
dy_launcher_get_property (GObject    *object,
                          unsigned    prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
    DyLauncher *launcher = DY_LAUNCHER (object);
    switch (prop_id) {
        case PROP_WEB_CONTEXT:
            g_value_set_object (value, dy_launcher_get_web_context (launcher));
            break;
        case PROP_WEB_VIEW:
            g_value_set_object (value, dy_launcher_get_web_view (launcher));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
dy_launcher_dispose (GObject *object)
{
    DyLauncher *launcher = DY_LAUNCHER (object);

    g_clear_object (&launcher->web_context);

    G_OBJECT_CLASS (dy_launcher_parent_class)->dispose (object);
}


static void
dy_launcher_class_init (DyLauncherClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = dy_launcher_dispose;
    object_class->get_property = dy_launcher_get_property;

    GApplicationClass *application_class = G_APPLICATION_CLASS (klass);
    application_class->open = dy_launcher_open;
    application_class->startup = dy_launcher_startup;
    application_class->activate = dy_launcher_activate;
    application_class->shutdown = dy_launcher_shutdown;

    s_properties[PROP_WEB_CONTEXT] =
        g_param_spec_object ("web-context",
                             "Web Context",
                             "The WebKitWebContext for the launcher",
                             WEBKIT_TYPE_WEB_CONTEXT,
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS);
    s_properties[PROP_WEB_VIEW] =
        g_param_spec_object ("web-view",
                             "Web View",
                             "The main WebKitWebView for the launcher",
                             WEBKIT_TYPE_WEB_VIEW,
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class,
                                       N_PROPERTIES,
                                       s_properties);

    s_signals[CREATE_WEB_VIEW] =
        g_signal_new ("create-web-view",
                      DY_TYPE_LAUNCHER,
                      G_SIGNAL_RUN_LAST,
                      0,
                      NULL,
                      NULL,
                      NULL,
                      WEBKIT_TYPE_WEB_VIEW,
                      0);

    s_actions[ACTION_QUIT] = make_action ("quit", on_action_quit);
    s_actions[ACTION_PREVIOUS] = make_action ("previous", on_action_prev);
    s_actions[ACTION_NEXT] = make_action ("next", on_action_next);
    s_actions[ACTION_RELOAD] = make_action ("reload", on_action_reload);
}


static void
dy_launcher_init (DyLauncher *launcher)
{
    for (size_t i = 0; i < N_ACTIONS; i++)
        g_action_map_add_action (G_ACTION_MAP (launcher), G_ACTION (s_actions[i]));
}


#ifndef DY_LAUNCHER_DEFAULT_APPID
# if DY_WEBKIT_GTK
#  define DY_LAUNCHER_DEFAULT_APPID "com.igalia.DinghyGtk"
# else
#  define DY_LAUNCHER_DEFAULT_APPID "com.igalia.Dinghy"
# endif
#endif

gpointer
dy_launcher_create_instance (gpointer user_data)
{
    /* Global singleton */
    const GApplicationFlags app_flags =
        G_APPLICATION_CAN_OVERRIDE_APP_ID |
        G_APPLICATION_HANDLES_OPEN ;
    return g_object_new (DY_TYPE_LAUNCHER,
                         "application-id", DY_LAUNCHER_DEFAULT_APPID,
                         "flags",  app_flags,
                         NULL);
}


DyLauncher*
dy_launcher_get_default (void)
{
    static GOnce create_instance_once = G_ONCE_INIT;
    g_once (&create_instance_once, dy_launcher_create_instance, NULL);
    g_assert_nonnull (create_instance_once.retval);
    return create_instance_once.retval;
}


WebKitWebView*
dy_launcher_get_web_view (DyLauncher *launcher)
{
    g_return_val_if_fail (launcher, NULL);
    return launcher->web_view;
}


WebKitWebContext*
dy_launcher_get_web_context (DyLauncher *launcher)
{
    g_return_val_if_fail (launcher, NULL);
    return launcher->web_context;
}
