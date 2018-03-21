/*
 * dinghyctl.c
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#define DY_INSIDE_DINGHY__ 1

#include "dy-config.h"
#include "dy-utils.h"

#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DY_DEFAULT_APPID
#  if DY_USE_WEBKITGTK
#    define DY_DEFAULT_APPID "com.igalia.DinghyGtk"
#  else
#    define DY_DEFAULT_APPID "com.igalia.Dinghy"
#  endif
#endif

#define GTK_ACTIONS_ACTIVATE "org.gtk.Actions", "Activate"
#define FDO_DBUS_PEER_PING   "org.freedesktop.DBus.Peer", "Ping"


static struct {
    char    *appid;
    char    *objpath;
    gboolean system_bus;
} s_options = {
    .appid = DY_DEFAULT_APPID,
};


static GOptionEntry s_cli_options[] = {
    { "appid", 'A', 0, G_OPTION_ARG_STRING, &s_options.appid,
        "Application identifier of the Dinghy instance to control",
        "ID" },
    { "object-path", 'o', 0, G_OPTION_ARG_STRING, &s_options.objpath,
        "Object path implementing the org.gtk.Actions interface",
        "PATH" },
    { "system", 'y', 0, G_OPTION_ARG_NONE, &s_options.system_bus,
        "Use the system bus instead of the session bus",
        NULL },
    { NULL, }
};


static gboolean
call_method (const char *iface,
             const char *method,
             GVariant   *params,
             GError    **error)
{
    const GBusType bus_type =
        s_options.system_bus ? G_BUS_TYPE_SYSTEM : G_BUS_TYPE_SESSION;
    g_autoptr(GDBusConnection) conn = g_bus_get_sync (bus_type, NULL, error);
    if (!error)
        return FALSE;

    g_autoptr(GVariant) result =
        g_dbus_connection_call_sync (conn,
                                     s_options.appid,
                                     s_options.objpath,
                                     iface,
                                     method,
                                     params,
                                     NULL,
                                     G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                     -1,
                                     NULL,
                                     error);
    return !!result;
}


struct cmd {
    const char *name;
    const char *desc;
    int (*handler) (const char *name, const void *data, int argc, char **argv);
    const void *data;
};

static const struct cmd* cmd_find_by_name (const char *name);


static void
cmd_check_simple_help (const char *name, int needed_argc, int *argc, char ***argv)
{
    const char *space = strchr (name, ' ');
    if (!space) space = strchr (name, '\0');

    g_autofree char *cmd_name = g_strndup (name, space - name);

    g_autoptr(GOptionContext) option_context = g_option_context_new (name);
    g_option_context_set_description (option_context,
                                      cmd_find_by_name (cmd_name)->desc);
    g_option_context_add_main_entries (option_context,
                                       &((GOptionEntry) { NULL, }),
                                       NULL);

    g_autoptr(GError) error = NULL;
    if (!g_option_context_parse (option_context, argc, argv, &error) || *argc > (1 + needed_argc)) {
        g_printerr ("%s: %s\n", name, error ? error->message : "No arguments expected");
        exit (EXIT_FAILURE);
    }
}


static int
cmd_generic_alias (const char *name,
                   const void *data,
                   int         argc,
                   char      **argv)
{
    const struct cmd *cmd = cmd_find_by_name ((const char*) data);
    return (*cmd->handler) (cmd->name, cmd->data, argc, argv);
}

static int
cmd_generic_no_args (const char               *name,
                     G_GNUC_UNUSED const void *data,
                     int                       argc,
                     char                    **argv)
{
    cmd_check_simple_help (name, 0, &argc, &argv);

    GVariant *params = g_variant_new ("(sava{sv})", name, NULL, NULL);

    g_autoptr(GError) error = NULL;
    if (!call_method (GTK_ACTIONS_ACTIVATE, params, &error)) {
        g_printerr ("%s\n", error->message);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int
cmd_appid (const char               *name,
           G_GNUC_UNUSED const void *data,
           int                       argc,
           char                    **argv)
{
    cmd_check_simple_help (name, 0, &argc, &argv);
    g_print ("%s\n", s_options.appid);
    return EXIT_SUCCESS;
}


static int
cmd_objpath (const char               *name,
             G_GNUC_UNUSED const void *data,
             int                       argc,
             char                    **argv)
{
    cmd_check_simple_help (name, 0, &argc, &argv);
    g_print ("%s\n", s_options.objpath);
    return EXIT_SUCCESS;
}


static int
cmd_open (const char               *name,
          G_GNUC_UNUSED const void *data,
          int                       argc,
          char                    **argv)
{
    cmd_check_simple_help ("open URL", 1, &argc, &argv);

    g_autoptr(GError) error = NULL;
    g_autofree char *utf8_uri =
        dy_uri_guess_from_user_input (argv[1], TRUE, &error);
    if (!utf8_uri) {
        g_printerr ("%s\n", error->message);
        return EXIT_FAILURE;
    }

    g_autoptr(GVariantBuilder) param_uri =
        g_variant_builder_new (G_VARIANT_TYPE ("av"));
    g_variant_builder_add (param_uri, "v", g_variant_new_string (utf8_uri));
    GVariant *params = g_variant_new ("(sava{sv})", "open", param_uri, NULL);

    if (!call_method (GTK_ACTIONS_ACTIVATE, params, &error)) {
        g_printerr ("%s\n", error->message);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}


static int
cmd_ping (const char               *name,
          G_GNUC_UNUSED const void *data,
          int                       argc,
          char                    **argv)
{
    cmd_check_simple_help (name, 0, &argc, &argv);
    return call_method (FDO_DBUS_PEER_PING, NULL, NULL)
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}


static int
cmd_help (const char *name,
          const void *data,
          int         argc,
          char      **argv)
{
    g_autoptr(GOptionContext) option_context =
        g_option_context_new ("help [command]");
    g_option_context_add_main_entries (option_context,
                                       &((GOptionEntry) { NULL, }),
                                       NULL);

    g_autoptr(GError) error = NULL;
    if (!g_option_context_parse (option_context, &argc, &argv, &error)) {
        g_printerr ("%s: %s\n", argv[0], error->message);
        return EXIT_FAILURE;
    }

    if (argc == 1) {
        g_print ("Available commands:\n");
        const struct cmd *cmds = data;
        for (unsigned i; cmds[i].name; i++) {
            if (cmds[i].desc) {
                g_print ("  %-10s %s\n", cmds[i].name, cmds[i].desc);
            }
        }
    } else if (argc == 2) {
        const struct cmd *cmd = cmd_find_by_name (argv[1]);
        if (!cmd) {
            g_printerr ("Unrecognized command: %s\n", argv[1]);
            return EXIT_FAILURE;
        }

        if (cmd->handler == cmd_generic_alias) {
            g_print ("'%s' is aliased to '%s'\n", argv[1],  (const char*) cmd->data);
        } else {
            char *cmd_argv[] = { (char*) cmd->name, "--help" };
            return (*cmd->handler) (cmd->name, cmd->data, G_N_ELEMENTS (cmd_argv), cmd_argv);
        }
    } else {
        g_printerr ("Invalid command line ussage\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


static const struct cmd*
cmd_find_by_name (const char *name)
{
    static const struct cmd cmdlist[] = {
        {
            .name = "appid",
            .desc = "Display application ID being remotely controlled",
            .handler = cmd_appid,
        },
        {
            .name = "objpath",
            .desc = "Display the D-Bus object path being used",
            .handler = cmd_objpath,
        },
        {
            .name = "help",
            .desc = "Obtain help about commands",
            .data = cmdlist,
            .handler = cmd_help,
        },
        {
            .name = "open",
            .desc = "Open an URL",
            .handler = cmd_open,
        },
        {
            .name = "previous",
            .desc = "Navigate backward in the page view history",
            .handler = cmd_generic_no_args,
        },
        {
            .name = "prev",
            .data = "previous",
            .handler = cmd_generic_alias,
        },
        {
            .name = "next",
            .desc = "Navigate forward in the page view history",
            .handler = cmd_generic_no_args,
        },
        {
            .name = "ping",
            .desc = "Check whether Dinghy is running",
            .handler = cmd_ping,
        },
        {
            .name = "quit",
            .desc = "Exit the application",
            .handler = cmd_generic_no_args,
        },
        {
            .name = "reload",
            .desc = "Reload the current page",
            .handler = cmd_generic_no_args,
        },
        {
            .name = NULL,
        },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS (cmdlist); i++)
        if (strcmp (cmdlist[i].name, name) == 0)
            return &cmdlist[i];

    return NULL;
}


static const char s_extended_help_text[] =
    "For a list of commands use:\n"
    "  dinghyctl help\n"
    "\n"
    "Help on each command can be obtained with one of:\n"
    "  dinghyctl help <command>\n"
    "  dinghyctl <command> --help\n";


int
main (int argc, char **argv)
{
    g_autoptr(GOptionContext) option_context =
        g_option_context_new ("<subcommand> [SUBCOMMAND-OPTION?]");
    g_option_context_set_description (option_context, s_extended_help_text);
    g_option_context_add_main_entries (option_context, s_cli_options, NULL);
    g_option_context_set_strict_posix (option_context, TRUE);

    g_autoptr(GError) error = NULL;
    if (!g_option_context_parse (option_context, &argc, &argv, &error)) {
        g_printerr ("Command line error: %s\n", error->message);
        return EXIT_FAILURE;
    }

    if (!g_application_id_is_valid (s_options.appid)) {
        g_printerr ("Invalid application ID: %s\n", s_options.appid);
        return EXIT_FAILURE;
    }

    if (!s_options.objpath) {
        s_options.objpath = dy_appid_to_dbus_object_path (s_options.appid);
    }
    if (!g_variant_is_object_path (s_options.objpath)) {
        g_printerr ("Invalid D-Bus object path: %s\n", s_options.objpath);
        return EXIT_FAILURE;
    }

    if (argc < 2) {
        g_printerr ("Missing subcommand\n");
        return EXIT_FAILURE;
    }

    const struct cmd *cmd = cmd_find_by_name (argv[1]);
    if (!cmd) {
        g_printerr ("Unrecognized command: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    return (*cmd->handler) (cmd->name, cmd->data, argc - 1, argv + 1);
}
