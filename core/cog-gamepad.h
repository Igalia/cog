/*
 * cog-gamepad.h
 * Copyright (C) 2022 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
#    error "Do not include this header directly, use <cog.h> instead" +
#endif

typedef struct wpe_view_backend *(GamepadProviderGetViewBackend) (void *, void *);

gboolean cog_gamepad_parse_backend(const char *name, GError **error);
void     cog_gamepad_set_backend(const char *name);
void     cog_gamepad_setup(GamepadProviderGetViewBackend *gamepad_get_view);
