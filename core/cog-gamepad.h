/*
 * cog-gamepad.h
 * Copyright (C) 2022 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
#    error "Do not include this header directly, use <cog.h> instead" +
#endif

typedef struct wpe_view_backend *(GamepadProviderGetViewBackend) (void *, void *);

void cog_register_gamepad_backend(GamepadProviderGetViewBackend *gamepad_get_view);
