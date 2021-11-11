/*
 * cog.c
 * Copyright (C) 2021 Igalia S.L.
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 * Copyright (C) 2017-2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-launcher.h"

static void
print_module_info(GIOExtension *extension, void *userdata G_GNUC_UNUSED)
{
    g_info("  %s - %d/%s",
           g_io_extension_get_name(extension),
           g_io_extension_get_priority(extension),
           g_type_name(g_io_extension_get_type(extension)));
}

int
main(int argc, char *argv[])
{
    g_set_application_name("Cog");
    cog_modules_add_directory(g_getenv("COG_MODULEDIR") ?: COG_MODULEDIR);

    g_info("%s:", COG_MODULES_PLATFORM_EXTENSION_POINT);
    cog_modules_foreach(COG_MODULES_PLATFORM, print_module_info, NULL);

    // We need to check whether we'll use automation mode before creating the launcher
    gboolean automated = FALSE;
    for (int i = 1; i < argc; i++) {
        if (g_str_equal("--automation", argv[i])) {
            automated = TRUE;
            break;
        }
    }

    CogSessionType          sessionType = automated ? COG_SESSION_AUTOMATED : COG_SESSION_REGULAR;
    g_autoptr(GApplication) app = G_APPLICATION(cog_launcher_new(sessionType));
    return g_application_run(app, argc, argv);
}
