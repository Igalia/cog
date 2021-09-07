/*
 * cog-gamepad-noop.c
 * Copyright (C) 2022 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#include "cog.h"
#include <glib.h>

void
cog_register_gamepad_backend(GamepadProviderGetViewBackend *gamepad_get_view G_GNUC_UNUSED)
{
}
