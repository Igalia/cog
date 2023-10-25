/*
 * cog-im-context-wl-v1.c
 * Copyright (C) 2020 Igalia S.L.
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-im-context-wl-v1.h"

#include <xkbcommon/xkbcommon.h>

static struct {
    struct zwp_text_input_v1 *text_input;
    struct wl_seat *seat;
    struct wl_surface *surface;
    struct wpe_view_backend *view_backend;
    WebKitInputMethodContext *context;
    bool active;
    bool panel_visible;

    uint32_t serial;
} wl_text_input = {
    NULL,
};

typedef struct {
    struct {
        char *text;
        GList *underlines;
        int32_t cursor_index;
    } preedit;

    struct {
        int32_t x;
        int32_t y;
        int32_t width;
        int32_t height;
    } cursor_rect;

    struct {
        char *text;
        uint32_t cursor_index;
        uint32_t anchor_index;
    } surrounding;

    struct {
        int32_t index;
        uint32_t length;
    } pending_surrounding_delete;

    struct {
        xkb_mod_mask_t shift_mask;
        xkb_mod_mask_t alt_mask;
        xkb_mod_mask_t control_mask;
    } modifiers;
} CogIMContextWlV1Private;

G_DEFINE_TYPE_WITH_PRIVATE(CogIMContextWlV1, cog_im_context_wl_v1, WEBKIT_TYPE_INPUT_METHOD_CONTEXT)

#define PRIV(obj) ((CogIMContextWlV1Private *) cog_im_context_wl_v1_get_instance_private(COG_IM_CONTEXT_WL_V1(obj)))

static char *
truncate_surrounding_if_needed(const char *text, uint32_t *cursor_index, uint32_t *anchor_index)
{
#define MAX_LEN 4000
    unsigned len = strlen (text);

    if (len < MAX_LEN)
        return NULL;

    const char *start, *end;
    if (*cursor_index < MAX_LEN && *anchor_index < MAX_LEN) {
        start = text;
        end = &text[MAX_LEN];
    } else if (*cursor_index > len - MAX_LEN && *anchor_index > len - MAX_LEN) {
        start = &text[len - MAX_LEN];
        end = &text[len];
    } else {
        unsigned selection_len = ABS (*cursor_index - *anchor_index);
        if (selection_len > MAX_LEN) {
            /* This is unsupported, let's just ignore the selection. */
            if (*cursor_index < MAX_LEN) {
                start = text;
                end = &text[MAX_LEN];
            } else if (*cursor_index > len - MAX_LEN) {
                start = &text[len - MAX_LEN];
                end = &text[len];
            } else {
                start = &text[MAX (0, *cursor_index - (MAX_LEN / 2))];
                end = &text[MIN (MAX_LEN, *cursor_index + (MAX_LEN / 2))];
            }
        } else {
            unsigned mid = MIN (*cursor_index, *anchor_index) + (selection_len / 2);
            start = &text[MAX (0, mid - (MAX_LEN / 2))];
            end = &text[MIN (MAX_LEN, mid + (MAX_LEN / 2))];
        }
    }

    if (start != text)
        start = g_utf8_next_char (start);
    if (end != &text[len])
        end = g_utf8_find_prev_char (text, end);

    *cursor_index -= start - text;
    *anchor_index -= start - text;

    return g_strndup (start, end - start);
#undef MAX_LEN
}

static void
cog_im_context_wl_v1_text_input_notify_surrounding(CogIMContextWlV1 *context)
{
    CogIMContextWlV1Private *priv = PRIV(context);
    char *truncated_text;
    uint32_t cursor_index;
    uint32_t anchor_index;

    if (!priv->surrounding.text)
        return;

    cursor_index = priv->surrounding.cursor_index;
    anchor_index = priv->surrounding.anchor_index;
    truncated_text = truncate_surrounding_if_needed (priv->surrounding.text,
                                                     &cursor_index,
                                                     &anchor_index);

    zwp_text_input_v1_set_surrounding_text(
        wl_text_input.text_input, truncated_text ? truncated_text : priv->surrounding.text, cursor_index, anchor_index);
    g_free(truncated_text);
}

static uint32_t
wk_input_purpose_to_wayland(WebKitInputPurpose purpose)
{
    switch (purpose) {
    case WEBKIT_INPUT_PURPOSE_FREE_FORM:
        return ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_NORMAL;
    case WEBKIT_INPUT_PURPOSE_DIGITS:
        return ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_DIGITS;
    case WEBKIT_INPUT_PURPOSE_NUMBER:
        return ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_NUMBER;
    case WEBKIT_INPUT_PURPOSE_PHONE:
        return ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_PHONE;
    case WEBKIT_INPUT_PURPOSE_URL:
        return ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_URL;
    case WEBKIT_INPUT_PURPOSE_EMAIL:
        return ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_EMAIL;
    case WEBKIT_INPUT_PURPOSE_PASSWORD:
        return ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_PASSWORD;
    }

    return ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_NORMAL;
}

static uint32_t
wk_input_hints_to_wayland(WebKitInputHints hints, WebKitInputPurpose purpose)
{
    uint32_t wl_hints = 0;

    if (hints & WEBKIT_INPUT_HINT_LOWERCASE)
        wl_hints |= ZWP_TEXT_INPUT_V1_CONTENT_HINT_LOWERCASE;
    if (hints & WEBKIT_INPUT_HINT_UPPERCASE_CHARS)
        wl_hints |= ZWP_TEXT_INPUT_V1_CONTENT_HINT_UPPERCASE;
    if (hints & WEBKIT_INPUT_HINT_UPPERCASE_WORDS)
        wl_hints |= ZWP_TEXT_INPUT_V1_CONTENT_HINT_TITLECASE;
    if (hints & WEBKIT_INPUT_HINT_UPPERCASE_SENTENCES)
        wl_hints |= ZWP_TEXT_INPUT_V1_CONTENT_HINT_AUTO_CAPITALIZATION;
    if (purpose == WEBKIT_INPUT_PURPOSE_PASSWORD) {
        wl_hints |= (ZWP_TEXT_INPUT_V1_CONTENT_HINT_HIDDEN_TEXT |
                     ZWP_TEXT_INPUT_V1_CONTENT_HINT_SENSITIVE_DATA);
    }

    return wl_hints;
}

static void
cog_im_context_wl_v1_text_input_notify_content_type(CogIMContextWlV1 *context)
{
    WebKitInputMethodContext *wk_context = WEBKIT_INPUT_METHOD_CONTEXT(context);
    WebKitInputPurpose purpose = webkit_input_method_context_get_input_purpose(wk_context);
    WebKitInputHints hints = webkit_input_method_context_get_input_hints(wk_context);

    zwp_text_input_v1_set_content_type(wl_text_input.text_input,
                                       wk_input_hints_to_wayland(hints, purpose),
                                       wk_input_purpose_to_wayland(purpose));
}

static void
cog_im_context_wl_v1_text_input_notify_cursor_rectangle(CogIMContextWlV1 *context)
{
    CogIMContextWlV1Private *priv = PRIV(context);

    zwp_text_input_v1_set_cursor_rectangle(wl_text_input.text_input,
                                           priv->cursor_rect.x,
                                           priv->cursor_rect.y,
                                           priv->cursor_rect.width,
                                           priv->cursor_rect.height);
}

static void
cog_im_context_wl_v1_text_input_commit_state(CogIMContextWlV1 *context)
{
    zwp_text_input_v1_commit_state(wl_text_input.text_input, ++wl_text_input.serial);
}

static void
cog_im_context_wl_v1_text_input_show_panel(CogIMContextWlV1 *context)
{
    WebKitInputHints hints = webkit_input_method_context_get_input_hints(WEBKIT_INPUT_METHOD_CONTEXT(context));
    bool can_show_panel = !(hints & WEBKIT_INPUT_HINT_INHIBIT_OSK);
    if (can_show_panel && !wl_text_input.panel_visible) {
        zwp_text_input_v1_show_input_panel(wl_text_input.text_input);
        wl_text_input.panel_visible = true;
    } else if (!can_show_panel && wl_text_input.panel_visible) {
        zwp_text_input_v1_hide_input_panel(wl_text_input.text_input);
        wl_text_input.panel_visible = false;
    }
}

static void
cog_im_context_wl_v1_text_input_hide_panel(void)
{
    if (!wl_text_input.panel_visible)
        return;

    zwp_text_input_v1_hide_input_panel (wl_text_input.text_input);
    wl_text_input.panel_visible = false;
}

static void
cog_im_context_wl_v1_text_input_activate(CogIMContextWlV1 *context)
{
    cog_im_context_wl_v1_text_input_show_panel(context);
    zwp_text_input_v1_activate(wl_text_input.text_input, wl_text_input.seat, wl_text_input.surface);
    cog_im_context_wl_v1_text_input_notify_surrounding(context);
    cog_im_context_wl_v1_text_input_notify_content_type(context);
    cog_im_context_wl_v1_text_input_notify_cursor_rectangle(context);
    cog_im_context_wl_v1_text_input_commit_state(context);
}

static void
cog_im_context_wl_v1_text_input_deactivate(CogIMContextWlV1 *context)
{
    zwp_text_input_v1_deactivate (wl_text_input.text_input,
                                  wl_text_input.seat);
}

static void
text_input_enter (void *data,
                  struct zwp_text_input_v1 *text_input,
                  struct wl_surface *surface)
{
    wl_text_input.active = true;
}

static void
text_input_leave(void *data, struct zwp_text_input_v1 *text_input)
{
    wl_text_input.active = false;
    cog_im_context_wl_v1_text_input_hide_panel();
}

static xkb_mod_mask_t
keysym_modifiers_get_mask(struct wl_array *map, const char *name)
{
    xkb_mod_index_t index = 0;
    const char *p = map->data;

    while (p < (const char *)map->data + map->size) {
        if (strcmp (p, name) == 0)
            return 1 << index;

        index++;
        p += strlen (p) + 1;
    }

    return XKB_MOD_INVALID;
}

static void
text_input_modifiers_map (void *data,
                          struct zwp_text_input_v1 *text_input,
                          struct wl_array *map)
{
    if (!wl_text_input.context)
        return;

    CogIMContextWlV1Private *priv = PRIV(wl_text_input.context);
    priv->modifiers.shift_mask = keysym_modifiers_get_mask(map, XKB_MOD_NAME_SHIFT);
    priv->modifiers.alt_mask = keysym_modifiers_get_mask(map, XKB_MOD_NAME_ALT);
    priv->modifiers.control_mask = keysym_modifiers_get_mask(map, XKB_MOD_NAME_CTRL);
}

static void
text_input_input_panel_state(void *data, struct zwp_text_input_v1 *text_input, uint32_t state)
{
}

static void
text_input_preedit_string (void *data,
                           struct zwp_text_input_v1 *text_input,
                           uint32_t serial,
                           const char *text,
                           const char *commit)
{
    if (!wl_text_input.context)
        return;

    CogIMContextWlV1Private *priv = PRIV(wl_text_input.context);
    bool valid = wl_text_input.serial == serial;
    if (valid && !priv->preedit.text)
        g_signal_emit_by_name(wl_text_input.context, "preedit-started");

    g_clear_pointer(&priv->preedit.text, g_free);
    priv->preedit.text = g_strdup(text);
    if (valid)
        g_signal_emit_by_name(wl_text_input.context, "preedit-changed");
}

static void
text_input_preedit_styling(void *data,
                           struct zwp_text_input_v1 *text_input,
                           uint32_t index,
                           uint32_t length,
                           uint32_t style)
{
    if (!wl_text_input.context)
        return;

    if (style == ZWP_TEXT_INPUT_V1_PREEDIT_STYLE_NONE)
        length = 0;

    WebKitInputMethodUnderline *underline = webkit_input_method_underline_new (index,
                                                                               index + length);
    switch (style) {
    case ZWP_TEXT_INPUT_V1_PREEDIT_STYLE_INCORRECT: {
        WebKitColor color = { 1., 0, 0, 1. };
        webkit_input_method_underline_set_color (underline, &color);
        break;
    }
    case ZWP_TEXT_INPUT_V1_PREEDIT_STYLE_HIGHLIGHT: {
        WebKitColor color = { 1., 1., 0., 1. };
        webkit_input_method_underline_set_color (underline, &color);
        break;
    }
    case ZWP_TEXT_INPUT_V1_PREEDIT_STYLE_ACTIVE: {
        WebKitColor color = { 0., 0., 1., 1. };
        webkit_input_method_underline_set_color (underline, &color);
        break;
    }
    case ZWP_TEXT_INPUT_V1_PREEDIT_STYLE_INACTIVE: {
        WebKitColor color = { 0.3, 0.3, 0.3, 1. };
        webkit_input_method_underline_set_color (underline, &color);
        break;
    }
    }

    CogIMContextWlV1Private *priv = PRIV(wl_text_input.context);
    priv->preedit.underlines = g_list_append(priv->preedit.underlines, underline);
}

static void
text_input_preedit_cursor(void *data, struct zwp_text_input_v1 *text_input, int32_t index)
{
    if (!wl_text_input.context)
        return;

    CogIMContextWlV1Private *priv = PRIV(wl_text_input.context);
    priv->preedit.cursor_index = index;
}

static void
text_input_commit_string(void *data, struct zwp_text_input_v1 *text_input, uint32_t serial, const char *text)
{
    if (!wl_text_input.context)
        return;

    CogIMContextWlV1Private *priv = PRIV(wl_text_input.context);
    bool valid = wl_text_input.serial == serial;
    if (valid && priv->preedit.text) {
        g_free(priv->preedit.text);
        priv->preedit.text = NULL;
        g_signal_emit_by_name(wl_text_input.context, "preedit-changed");
        g_signal_emit_by_name(wl_text_input.context, "preedit-finished");
    } else {
        g_clear_pointer(&priv->preedit.text, g_free);
    }

    if (valid && priv->surrounding.text && priv->pending_surrounding_delete.length) {
        char *pos = priv->surrounding.text + priv->surrounding.cursor_index;
        glong char_index = g_utf8_pointer_to_offset (priv->surrounding.text, pos);
        pos += priv->pending_surrounding_delete.index;
        glong char_start = g_utf8_pointer_to_offset (priv->surrounding.text, pos);
        pos += priv->pending_surrounding_delete.length;
        glong char_end = g_utf8_pointer_to_offset (priv->surrounding.text, pos);

        g_signal_emit_by_name(wl_text_input.context, "delete-surrounding", char_start - char_index,
                              char_end - char_start);
    }
    priv->pending_surrounding_delete.index = 0;
    priv->pending_surrounding_delete.length = 0;

    if (valid && text)
        g_signal_emit_by_name (wl_text_input.context, "committed", text);
}


static void
text_input_cursor_position (void *data,
                            struct zwp_text_input_v1 *text_input,
                            int32_t index,
                            int32_t anchor)
{
}


static void
text_input_delete_surrounding_text (void *data,
                                    struct zwp_text_input_v1 *text_input,
                                    int32_t index,
                                    uint32_t length)
{
    if (!wl_text_input.context)
        return;

    CogIMContextWlV1Private *priv = PRIV(wl_text_input.context);
    priv->pending_surrounding_delete.index = index;
    priv->pending_surrounding_delete.length = length;
}

static void
text_input_keysym(void *data,
                  struct zwp_text_input_v1 *text_input,
                  uint32_t serial,
                  uint32_t time,
                  uint32_t sym,
                  uint32_t state,
                  uint32_t modifiers)
{
    if (!wl_text_input.view_backend)
        return;

    CogIMContextWlV1Private *priv = PRIV(wl_text_input.context);
    uint32_t wpe_modifiers = 0;
    if (modifiers & priv->modifiers.shift_mask)
        wpe_modifiers |= wpe_input_keyboard_modifier_shift;
    if (modifiers & priv->modifiers.alt_mask)
        wpe_modifiers |= wpe_input_keyboard_modifier_alt;
    if (modifiers & priv->modifiers.control_mask)
        wpe_modifiers |= wpe_input_keyboard_modifier_control;

    struct wpe_input_keyboard_event event = { time, sym, 0, state == true, wpe_modifiers };
    wpe_view_backend_dispatch_keyboard_event (wl_text_input.view_backend, &event);
}


static void
text_input_language (void *data,
                     struct zwp_text_input_v1 *text_input,
                     uint32_t serial,
                     const char *language)
{
}

static void
text_input_text_direction (void *data,
                           struct zwp_text_input_v1 *text_input,
                           uint32_t serial,
                           uint32_t direction)
{
}

static const struct zwp_text_input_v1_listener text_input_listener = {
    .enter = text_input_enter,
    .leave = text_input_leave,
    .modifiers_map = text_input_modifiers_map,
    .input_panel_state = text_input_input_panel_state,
    .preedit_string = text_input_preedit_string,
    .preedit_styling = text_input_preedit_styling,
    .preedit_cursor = text_input_preedit_cursor,
    .commit_string = text_input_commit_string,
    .cursor_position = text_input_cursor_position,
    .delete_surrounding_text = text_input_delete_surrounding_text,
    .keysym = text_input_keysym,
    .language = text_input_language,
    .text_direction = text_input_text_direction,
};

static void
cog_im_context_wl_v1_finalize(GObject *object)
{
    CogIMContextWlV1Private *priv = PRIV(object);

    g_free(priv->preedit.text);
    g_free(priv->surrounding.text);

    G_OBJECT_CLASS(cog_im_context_wl_v1_parent_class)->finalize(object);
}

static void
cog_im_context_wl_v1_get_preedit(WebKitInputMethodContext *context,
                                 char **text,
                                 GList **underlines,
                                 guint *cursor_offset)
{
    CogIMContextWlV1Private *priv = PRIV(context);

    if (text)
        *text = priv->preedit.text ? g_strdup(priv->preedit.text) : g_strdup("");

    if (underlines)
        *underlines = priv->preedit.underlines;
    else
        g_list_free_full(priv->preedit.underlines, g_object_unref);
    priv->preedit.underlines = NULL;

    if (cursor_offset)
        *cursor_offset = priv->preedit.cursor_index;
}

static void
cog_im_context_wl_v1_notify_focus_in(WebKitInputMethodContext *context)
{
    if (wl_text_input.context == context)
        return;

    if (!wl_text_input.text_input)
        return;

    wl_text_input.context = context;
    cog_im_context_wl_v1_text_input_activate(COG_IM_CONTEXT_WL_V1(context));
}

static void
cog_im_context_wl_v1_notify_focus_out(WebKitInputMethodContext *context)
{
    if (wl_text_input.context != context)
        return;

    cog_im_context_wl_v1_text_input_deactivate(COG_IM_CONTEXT_WL_V1(context));
    wl_text_input.context = NULL;
}

static void
cog_im_context_wl_v1_notify_cursor_area(WebKitInputMethodContext *context, int x, int y, int width, int height)
{
    if (!wl_text_input.active)
        return;

    CogIMContextWlV1Private *priv = PRIV(context);

    if (priv->cursor_rect.x == x && priv->cursor_rect.y == y && priv->cursor_rect.width == width &&
        priv->cursor_rect.height == height)
        return;

    priv->cursor_rect.x = x;
    priv->cursor_rect.y = y;
    priv->cursor_rect.width = width;
    priv->cursor_rect.height = height;

    if (wl_text_input.context == context) {
        cog_im_context_wl_v1_text_input_notify_cursor_rectangle(COG_IM_CONTEXT_WL_V1(context));
        cog_im_context_wl_v1_text_input_commit_state(COG_IM_CONTEXT_WL_V1(context));
    }
}

static void
cog_im_context_wl_v1_notify_surrounding(WebKitInputMethodContext *context,
                                        const char *text,
                                        guint length,
                                        guint cursor_index,
                                        guint selection_index)
{
    if (!wl_text_input.active)
        return;

    CogIMContextWlV1Private *priv = PRIV(context);
    g_clear_pointer(&priv->surrounding.text, g_free);
    priv->surrounding.text = g_strndup (text, length);
    priv->surrounding.cursor_index = cursor_index;
    priv->surrounding.anchor_index = selection_index;

    if (wl_text_input.context == context)
        cog_im_context_wl_v1_text_input_notify_surrounding(COG_IM_CONTEXT_WL_V1(context));
}

static void
cog_im_context_wl_v1_reset(WebKitInputMethodContext *context)
{
    if (wl_text_input.context != context || !wl_text_input.active)
        return;

    zwp_text_input_v1_reset (wl_text_input.text_input);
    cog_im_context_wl_v1_text_input_notify_surrounding(COG_IM_CONTEXT_WL_V1(context));
}

static void
cog_im_context_wl_v1_class_init(CogIMContextWlV1Class *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
    gobject_class->finalize = cog_im_context_wl_v1_finalize;

    WebKitInputMethodContextClass *im_context_class = WEBKIT_INPUT_METHOD_CONTEXT_CLASS (klass);
    im_context_class->get_preedit = cog_im_context_wl_v1_get_preedit;
    im_context_class->notify_focus_in = cog_im_context_wl_v1_notify_focus_in;
    im_context_class->notify_focus_out = cog_im_context_wl_v1_notify_focus_out;
    im_context_class->notify_cursor_area = cog_im_context_wl_v1_notify_cursor_area;
    im_context_class->notify_surrounding = cog_im_context_wl_v1_notify_surrounding;
    im_context_class->reset = cog_im_context_wl_v1_reset;
}

static void
cog_im_context_wl_v1_content_type_changed(CogIMContextWlV1 *context)
{
    if (wl_text_input.context != WEBKIT_INPUT_METHOD_CONTEXT(context))
        return;

    cog_im_context_wl_v1_text_input_notify_content_type(context);
    cog_im_context_wl_v1_text_input_commit_state(context);
    cog_im_context_wl_v1_text_input_show_panel(context);
}

static void
cog_im_context_wl_v1_init(CogIMContextWlV1 *context)
{
    g_signal_connect_swapped(context, "notify::input-purpose", G_CALLBACK(cog_im_context_wl_v1_content_type_changed),
                             context);
    g_signal_connect_swapped(context, "notify::input-hints", G_CALLBACK(cog_im_context_wl_v1_content_type_changed),
                             context);
}

void
cog_im_context_wl_v1_set_text_input(struct zwp_text_input_v1 *text_input,
                                    struct wl_seat *seat,
                                    struct wl_surface *surface)
{
    g_clear_pointer(&wl_text_input.text_input, zwp_text_input_v1_destroy);
    wl_text_input.text_input = text_input;
    wl_text_input.seat = seat;
    wl_text_input.surface = surface;
    wl_text_input.serial = 0;
    if (wl_text_input.text_input)
        zwp_text_input_v1_add_listener(wl_text_input.text_input, &text_input_listener, NULL);
}

void
cog_im_context_wl_v1_set_view_backend(struct wpe_view_backend *backend)
{
    wl_text_input.view_backend = backend;
}

WebKitInputMethodContext *
cog_im_context_wl_v1_new(void)
{
    return WEBKIT_INPUT_METHOD_CONTEXT(g_object_new(COG_TYPE_IM_CONTEXT_WL_V1, NULL));
}
