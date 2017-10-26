/*
 * dy-launcher.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "dy-launcher.h"
#include "dy-webkit-utils.h"


struct _DyLauncher {
    GApplication      parent;
    WebKitWebContext *web_context;
    WebKitWebView    *web_view;
};


G_DEFINE_TYPE (DyLauncher, dy_launcher, G_TYPE_APPLICATION)


enum {
    CREATE_WEB_VIEW,
    LAST_SIGNAL,
};

static int s_signals[LAST_SIGNAL] = { 0, };


enum {
    PROP_0,
    PROP_WEB_CONTEXT,
    N_PROPERTIES
};

static GParamSpec *s_properties[N_PROPERTIES] = { NULL, };


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
    g_signal_emit (launcher,
                   s_signals[CREATE_WEB_VIEW],
                   0,
                   &launcher->web_view);

    /*
     * Create the web view ourselves if the signal handler did not.
     */
    if (!launcher->web_view) {
        launcher->web_view = webkit_web_view_new_with_context (dy_launcher_get_web_context (launcher));
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
}


static void
dy_launcher_init (DyLauncher *launcher)
{
}


gpointer
dy_launcher_create_instance (gpointer user_data)
{
    /* Global singleton */
    const GApplicationFlags app_flags =
        G_APPLICATION_CAN_OVERRIDE_APP_ID |
        G_APPLICATION_HANDLES_OPEN ;
    return g_object_new (DY_TYPE_LAUNCHER,
                         "application-id", NULL  /* TODO */,
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


WebKitWebContext*
dy_launcher_get_web_context (DyLauncher *launcher)
{
    g_return_val_if_fail (launcher, NULL);
    return launcher->web_context;
}
