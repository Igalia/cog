/*
 * cog-popup-menu-fdo.h
 * Copyright (C) 2020 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#include <glib.h>
#include <wayland-client.h>

typedef struct _CogPopupMenu CogPopupMenu;
typedef struct _WebKitOptionMenu WebKitOptionMenu;
struct wpe_input_pointer_event;

guint
cog_popup_menu_get_height_for_option_menu (WebKitOptionMenu *option_menu);

CogPopupMenu *
cog_popup_menu_create (WebKitOptionMenu *option_menu, struct wl_shm *shm, int width, int height, int scale);

void
cog_popup_menu_destroy (CogPopupMenu *popup_menu);

void
cog_popup_menu_handle_event (CogPopupMenu *popup_menu, int state, int x_coord, int y_coord);

gboolean
cog_popup_menu_has_final_selection (CogPopupMenu *popup_menu, int *selected_index);

struct wl_buffer *
cog_popup_menu_get_buffer (CogPopupMenu *popup_menu);
