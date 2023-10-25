/*
 * cog-gamepad.c
 * Copyright (C) 2022 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog.h"
#include <wpe/wpe.h>

#if COG_ENABLE_GAMEPAD_MANETTE
extern const struct wpe_gamepad_provider_interface s_manette_provider_interface;
extern const struct wpe_gamepad_interface          s_manette_device_interface;
#endif

static const struct CogGamepadBackend {
    const char                                  *name;
    const struct wpe_gamepad_provider_interface *provider;
    const struct wpe_gamepad_interface          *device;
} s_gamepad_backends[] = {
#if COG_ENABLE_GAMEPAD_MANETTE
    {
        "manette",
        &s_manette_provider_interface,
        &s_manette_device_interface,
    },
#endif
    {
        "none",
        NULL,
        NULL,
    },
};

/* selected gamepad backend interfaces */
static const struct CogGamepadBackend       *s_backend = &s_gamepad_backends[0];
#if COG_ENABLE_GAMEPAD_MANETTE
static struct wpe_gamepad_provider_interface s_provider_interface;
#endif

void
cog_gamepad_set_backend(const char *name)
{
    static bool initialized = false;

    g_return_if_fail(!initialized);

    if (!name)
        goto bail;

    for (int i = 0; i < G_N_ELEMENTS(s_gamepad_backends); i++) {
        if (g_strcmp0(name, s_gamepad_backends[i].name) == 0) {
            s_backend = &s_gamepad_backends[i];
            break;
        }
    }

bail:
    g_debug("gamepad backend: %s", s_backend->name);
    initialized = true;
}

void
cog_gamepad_setup(GamepadProviderGetViewBackend *gamepad_get_view)
{
    static bool initialized = false;

    g_return_if_fail(!initialized);

    g_debug("gamepad setup: %s", s_backend->name);
    if (!s_backend->provider)
        return;

#if COG_ENABLE_GAMEPAD_MANETTE
    /* to add gamepad_get_view, use a non-const temporal provider interface */
    s_provider_interface = (struct wpe_gamepad_provider_interface) * s_backend->provider;
    s_provider_interface.get_view_backend = gamepad_get_view;

    wpe_gamepad_set_handler(&s_provider_interface, s_backend->device);
#endif

    initialized = true;
}

gboolean
cog_gamepad_parse_backend(const char *name, GError **error)
{
    g_autoptr(GString) list = g_string_new("");

    for (int i = 0; i < G_N_ELEMENTS(s_gamepad_backends); i++) {
        if (g_strcmp0(name, s_gamepad_backends[i].name) == 0)
            return TRUE;
        if (list->len == 0) /* default */
            g_string_append_printf(list, "*%s", s_gamepad_backends[i].name);
        else
            g_string_append_printf(list, ", %s", s_gamepad_backends[i].name);
    }

    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                "Invalid gamepad implementation: '%s'. Options: [ %s ]", name, list->str);
    return FALSE;
}
