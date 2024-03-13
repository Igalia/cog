/*
 * cog-popup-menu-wl.c
 * Copyright (C) 2020 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-popup-menu-wl.h"

#include "os-compatibility.h"
#include <cairo.h>
#include <sys/mman.h>
#include <unistd.h>

#include <stdio.h>

struct _CogPopupMenu {
    WebKitOptionMenu *option_menu;

    int shm_pool_fd;
    int shm_pool_size;
    void *shm_pool_data;
    struct wl_shm_pool *shm_pool;

    int width;
    int height;
    int scale;
    int stride;

    struct wl_buffer *buffer;

    cairo_surface_t *cr_surface;
    cairo_t *cr;

    int menu_item_width;

    guint menu_num_items;

    bool menu_has_paging;
    unsigned menu_current_page;
    unsigned menu_max_page;

    int initial_selection_index;
    bool finalized_selection;
    int finalized_local_index;
    int finalized_selection_index;
    bool pending_changes;
};

#define VERTICAL_PADDING             20
#define HORIZONTAL_PADDING           40
#define ITEM_HEIGHT                  40
#define ITEM_TEXT_VERTICAL_ORIGIN    10
#define ITEM_TEXT_HORIZONTAL_PADDING 10
#define ITEM_TEXT_SIZE               18

static void
cog_popup_menu_paint(CogPopupMenu *popup_menu)
{
    {
        cairo_set_source_rgba(popup_menu->cr, 0.8, 0.8, 0.8, 1);
        cairo_rectangle(popup_menu->cr, 0, 0, popup_menu->width, popup_menu->height);
        cairo_fill(popup_menu->cr);
    }

    {
        cairo_save(popup_menu->cr);
        cairo_scale(popup_menu->cr, popup_menu->scale, popup_menu->scale);
        cairo_translate(popup_menu->cr, HORIZONTAL_PADDING, VERTICAL_PADDING);

        cairo_set_line_width(popup_menu->cr, 1);
        cairo_set_font_size(popup_menu->cr, ITEM_TEXT_SIZE);

        if (popup_menu->menu_has_paging) {
            cairo_rectangle(popup_menu->cr, 0, 0, (popup_menu->menu_item_width / popup_menu->scale), ITEM_HEIGHT);

            cairo_set_source_rgba(popup_menu->cr, 0.8, 0.8, 0.8, 1);
            cairo_fill_preserve(popup_menu->cr);
            cairo_set_source_rgba(popup_menu->cr, 0, 0, 0, 1);
            cairo_stroke(popup_menu->cr);

            cairo_move_to(popup_menu->cr, (popup_menu->menu_item_width / popup_menu->scale) / 2, 10);
            cairo_line_to(popup_menu->cr, (popup_menu->menu_item_width / popup_menu->scale) / 2 - 20, 30);
            cairo_line_to(popup_menu->cr, (popup_menu->menu_item_width / popup_menu->scale) / 2 + 20, 30);
            cairo_close_path(popup_menu->cr);

            cairo_set_source_rgba(popup_menu->cr, 0, 0, 0, 1);
            cairo_fill(popup_menu->cr);

            cairo_translate(popup_menu->cr, 0, ITEM_HEIGHT);
        }

        guint i = popup_menu->menu_current_page * 5;
        guint i_max = i + (popup_menu->menu_has_paging ? 5 : 7);
        guint i_end = MIN(i_max, webkit_option_menu_get_n_items(popup_menu->option_menu));
        for (; i < i_end; ++i) {
            WebKitOptionMenuItem *item = webkit_option_menu_get_item(popup_menu->option_menu, i);

            cairo_rectangle(popup_menu->cr, 0, 0, (popup_menu->menu_item_width / popup_menu->scale), ITEM_HEIGHT);

            if (!webkit_option_menu_item_is_enabled(item)) {
                cairo_set_source_rgba(popup_menu->cr, 0.6, 0.6, 0.6, 1);
            } else if (i == popup_menu->finalized_selection_index) {
                cairo_set_source_rgba(popup_menu->cr, 0.3, 0.7, 1, 1);
            } else if (webkit_option_menu_item_is_selected(item)) {
                cairo_set_source_rgba(popup_menu->cr, 0.6, 0.8, 1, 1);
            } else {
                cairo_set_source_rgba(popup_menu->cr, 1, 1, 1, 1);
            }

            cairo_fill_preserve(popup_menu->cr);
            cairo_set_source_rgba(popup_menu->cr, 0, 0, 0, 1);
            cairo_stroke(popup_menu->cr);

            const gchar *label = webkit_option_menu_item_get_label(item);
            {
                cairo_save(popup_menu->cr);

                cairo_translate(popup_menu->cr,
                                5 + ITEM_TEXT_HORIZONTAL_PADDING,
                                (ITEM_HEIGHT - ITEM_TEXT_VERTICAL_ORIGIN));
                cairo_show_text(popup_menu->cr, label);

                cairo_restore(popup_menu->cr);
            }

            cairo_translate(popup_menu->cr, 0, ITEM_HEIGHT);
        }

        for (; i < i_max; ++i) {
            cairo_translate(popup_menu->cr, 0, ITEM_HEIGHT);
        }

        if (popup_menu->menu_has_paging) {
            cairo_rectangle(popup_menu->cr, 0, 0, (popup_menu->menu_item_width / popup_menu->scale), ITEM_HEIGHT);
            cairo_set_source_rgba(popup_menu->cr, 0.8, 0.8, 0.8, 1);

            cairo_fill_preserve(popup_menu->cr);
            cairo_set_source_rgba(popup_menu->cr, 0, 0, 0, 1);
            cairo_stroke(popup_menu->cr);

            cairo_move_to(popup_menu->cr, (popup_menu->menu_item_width / popup_menu->scale) / 2, 30);
            cairo_line_to(popup_menu->cr, (popup_menu->menu_item_width / popup_menu->scale) / 2 - 20, 10);
            cairo_line_to(popup_menu->cr, (popup_menu->menu_item_width / popup_menu->scale) / 2 + 20, 10);
            cairo_close_path(popup_menu->cr);

            cairo_set_source_rgba(popup_menu->cr, 0, 0, 0, 1);
            cairo_fill(popup_menu->cr);
        }

        cairo_restore(popup_menu->cr);
    }
}

guint
cog_popup_menu_get_height_for_option_menu(WebKitOptionMenu *option_menu)
{
    guint n_items = webkit_option_menu_get_n_items(option_menu);
    return 2 * VERTICAL_PADDING + MIN(n_items, 7) * ITEM_HEIGHT;
}

CogPopupMenu *
cog_popup_menu_create(WebKitOptionMenu *option_menu, struct wl_shm *shm, int width, int height, int scale)
{
    CogPopupMenu *popup_menu = g_new0(CogPopupMenu, 1);

    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width * scale);
    popup_menu->shm_pool_size = (height * scale) * stride;
    popup_menu->shm_pool_fd = os_create_anonymous_file(popup_menu->shm_pool_size);
    if (popup_menu->shm_pool_fd < 0) {
        g_free(popup_menu);
        return NULL;
    }

    popup_menu->shm_pool_data =
        mmap(NULL, popup_menu->shm_pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, popup_menu->shm_pool_fd, 0);
    if (popup_menu->shm_pool_data == MAP_FAILED) {
        close(popup_menu->shm_pool_fd);
        g_free(popup_menu);
        return NULL;
    }

    popup_menu->option_menu = option_menu;

    popup_menu->shm_pool = wl_shm_create_pool(shm, popup_menu->shm_pool_fd, popup_menu->shm_pool_size);
    popup_menu->width = width * scale;
    popup_menu->height = height * scale;
    popup_menu->scale = scale;
    popup_menu->stride = stride;

    popup_menu->menu_item_width = popup_menu->width - 2 * HORIZONTAL_PADDING * popup_menu->scale;

    popup_menu->cr_surface = cairo_image_surface_create_for_data(popup_menu->shm_pool_data,
                                                                 CAIRO_FORMAT_ARGB32,
                                                                 popup_menu->width,
                                                                 popup_menu->height,
                                                                 popup_menu->stride);
    popup_menu->cr = cairo_create(popup_menu->cr_surface);

    guint n_items = webkit_option_menu_get_n_items(option_menu);
    popup_menu->menu_num_items = MIN(n_items, 7);
    popup_menu->menu_has_paging = n_items > 7;
    popup_menu->menu_current_page = 0;
    popup_menu->menu_max_page = MAX(0, ((gint) n_items - 1) / 5);

    popup_menu->initial_selection_index = -1;
    popup_menu->finalized_selection = false;
    popup_menu->finalized_local_index = -1;
    popup_menu->finalized_selection_index = -1;
    popup_menu->pending_changes = false;

    {
        for (guint i = 0; i < n_items; ++i) {
            WebKitOptionMenuItem *item = webkit_option_menu_get_item(option_menu, i);
            if (webkit_option_menu_item_is_selected(item)) {
                popup_menu->initial_selection_index = i;
                break;
            }
        }
    }

    cog_popup_menu_paint(popup_menu);

    return popup_menu;
}

void
cog_popup_menu_destroy(CogPopupMenu *popup_menu)
{
    g_clear_pointer(&popup_menu->cr, cairo_destroy);
    g_clear_pointer(&popup_menu->cr_surface, cairo_surface_destroy);

    g_clear_pointer(&popup_menu->buffer, wl_buffer_destroy);
    g_clear_pointer(&popup_menu->shm_pool, wl_shm_pool_destroy);

    munmap(popup_menu->shm_pool_data, popup_menu->shm_pool_size);
    close(popup_menu->shm_pool_fd);

    g_free(popup_menu);
}

void
cog_popup_menu_handle_event(CogPopupMenu *popup_menu, int state, int x_coord, int y_coord)
{
    int local_index = -1;
    {
        int descaled_y = y_coord / popup_menu->scale;

        for (guint i = 0; i < popup_menu->menu_num_items; ++i) {
            if (x_coord > HORIZONTAL_PADDING * popup_menu->scale &&
                x_coord < (popup_menu->width - HORIZONTAL_PADDING * popup_menu->scale) &&
                descaled_y > (VERTICAL_PADDING + i * ITEM_HEIGHT) &&
                descaled_y < (VERTICAL_PADDING + (i + 1) * ITEM_HEIGHT)) {
                local_index = i;
                break;
            }
        }
    }

    int selection_index = popup_menu->menu_current_page * 5 + local_index;
    if (popup_menu->menu_has_paging) {
        if (local_index == 0 || local_index == 6)
            selection_index = -1;
        else
            selection_index = selection_index - 1;
    }
    if (selection_index >= webkit_option_menu_get_n_items(popup_menu->option_menu))
        selection_index = -1;
    if (local_index != -1 && selection_index != -1) {
        WebKitOptionMenuItem *item = webkit_option_menu_get_item(popup_menu->option_menu, selection_index);
        if (!item || !webkit_option_menu_item_is_enabled(item))
            return;
    }

    if (state == COG_POPUP_MENU_EVENT_STATE_PRESSED) {
        popup_menu->finalized_local_index = local_index;
        if (local_index == -1)
            popup_menu->finalized_selection_index = -1;
        else
            popup_menu->finalized_selection_index = selection_index;

        popup_menu->pending_changes = true;
    } else {
        if (local_index == popup_menu->finalized_local_index) {
            if (popup_menu->menu_has_paging && local_index == 0) {
                popup_menu->menu_current_page = MAX((gint) popup_menu->menu_current_page - 1, 0);
                popup_menu->pending_changes = true;
            } else if (popup_menu->menu_has_paging && local_index == 6) {
                popup_menu->menu_current_page = MIN(popup_menu->menu_current_page + 1, popup_menu->menu_max_page);
                popup_menu->pending_changes = true;
            } else {
                popup_menu->finalized_selection = true;
                if (selection_index == -1)
                    popup_menu->finalized_selection_index = popup_menu->initial_selection_index;
                popup_menu->pending_changes = false;
            }
        } else
            popup_menu->pending_changes = true;
    }
}

gboolean
cog_popup_menu_has_final_selection(CogPopupMenu *popup_menu, int *selected_index)
{
    if (popup_menu->finalized_selection) {
        *selected_index = popup_menu->finalized_selection_index;
        return true;
    }

    *selected_index = popup_menu->initial_selection_index;
    return false;
}

struct wl_buffer *
cog_popup_menu_get_buffer(CogPopupMenu *popup_menu)
{
    if (popup_menu->pending_changes) {
        popup_menu->pending_changes = false;
        cog_popup_menu_paint(popup_menu);
    }

    if (popup_menu->buffer == NULL) {
        popup_menu->buffer = wl_shm_pool_create_buffer(popup_menu->shm_pool, 0, popup_menu->width, popup_menu->height,
                                                       popup_menu->stride, WL_SHM_FORMAT_ARGB8888);
    }

    return popup_menu->buffer;
}
