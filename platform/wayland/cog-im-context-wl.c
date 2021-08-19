/*
 * cog-im-context-wl.c
 * Copyright (C) 2020 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#include "cog-im-context-wl.h"

static struct {
    struct zwp_text_input_v3 *text_input;
    WebKitInputMethodContext *context;
    bool focused;
    uint32_t serial;
} wl_text_input = {
    NULL,
};

typedef struct {
    char *text;
    int32_t cursor_begin;
    int32_t cursor_end;
} Preedit;

typedef struct {
    Preedit pending_preedit;
    Preedit current_preedit;

    char *pending_commit;

    struct {
        int32_t x;
        int32_t y;
        int32_t width;
        int32_t height;
    } cursor_rect;

    struct {
        char *text;
        int32_t cursor_index;
        int32_t anchor_index;
    } surrounding;

    enum zwp_text_input_v3_change_cause text_change_cause;

    struct {
        uint32_t before_length;
        uint32_t after_length;
    } pending_surrounding_delete;
} CogIMContextWlPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(CogIMContextWl, cog_im_context_wl, WEBKIT_TYPE_INPUT_METHOD_CONTEXT)

#define PRIV(obj) ((CogIMContextWlPrivate *) cog_im_context_wl_get_instance_private(COG_IM_CONTEXT_WL(obj)))

static char *
truncate_surrounding_if_needed(const char *text, int32_t *cursor_index, int32_t *anchor_index)
{
#define MAX_LEN 4000
    int len = strlen(text);

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
        int selection_len = ABS (*cursor_index - *anchor_index);
        if (selection_len > MAX_LEN) {
            /* This is unsupported, let's just ignore the selection. */
            if (*cursor_index < MAX_LEN) {
                start = text;
                end = &text[MAX_LEN];
            } else if (*cursor_index > len - MAX_LEN) {
                start = &text[len - MAX_LEN];
                end = &text[len];
            } else {
                start = &text[MAX(0, *cursor_index - (MAX_LEN / 2))];
                end = &text[MIN(MAX_LEN, *cursor_index + (MAX_LEN / 2))];
            }
        } else {
            int mid = MIN(*cursor_index, *anchor_index) + (selection_len / 2);
            start = &text[MAX(0, mid - (MAX_LEN / 2))];
            end = &text[MIN(MAX_LEN, mid + (MAX_LEN / 2))];
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
cog_im_context_wl_text_input_notify_surrounding(CogIMContextWl *context)
{
    CogIMContextWlPrivate *priv = PRIV(context);
    char *truncated_text;
    int32_t cursor_index;
    int32_t anchor_index;

    if (!priv->surrounding.text)
        return;

    cursor_index = priv->surrounding.cursor_index;
    anchor_index = priv->surrounding.anchor_index;
    truncated_text = truncate_surrounding_if_needed (priv->surrounding.text,
                                                     &cursor_index,
                                                     &anchor_index);

    zwp_text_input_v3_set_surrounding_text (wl_text_input.text_input,
                                            truncated_text ? truncated_text : priv->surrounding.text,
                                            cursor_index, anchor_index);
    zwp_text_input_v3_set_text_change_cause (wl_text_input.text_input,
                                             priv->text_change_cause);
    g_free(truncated_text);
}

static uint32_t
wk_input_purpose_to_wayland(WebKitInputPurpose purpose)
{
    switch (purpose) {
    case WEBKIT_INPUT_PURPOSE_FREE_FORM:
        return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL;
    case WEBKIT_INPUT_PURPOSE_DIGITS:
        return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_DIGITS;
    case WEBKIT_INPUT_PURPOSE_NUMBER:
        return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NUMBER;
    case WEBKIT_INPUT_PURPOSE_PHONE:
        return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PHONE;
    case WEBKIT_INPUT_PURPOSE_URL:
        return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_URL;
    case WEBKIT_INPUT_PURPOSE_EMAIL:
        return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_EMAIL;
    case WEBKIT_INPUT_PURPOSE_PASSWORD:
        return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD;
    }

    return ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL;
}

static uint32_t
wk_input_hints_to_wayland(WebKitInputHints hints, WebKitInputPurpose purpose)
{
    uint32_t wl_hints = 0;

    if (hints & WEBKIT_INPUT_HINT_SPELLCHECK)
        wl_hints |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_SPELLCHECK;
    if (hints & WEBKIT_INPUT_HINT_LOWERCASE)
        wl_hints |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_LOWERCASE;
    if (hints & WEBKIT_INPUT_HINT_UPPERCASE_CHARS)
        wl_hints |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_UPPERCASE;
    if (hints & WEBKIT_INPUT_HINT_UPPERCASE_WORDS)
        wl_hints |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_TITLECASE;
    if (hints & WEBKIT_INPUT_HINT_UPPERCASE_SENTENCES)
        wl_hints |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_AUTO_CAPITALIZATION;
    if (purpose == WEBKIT_INPUT_PURPOSE_PASSWORD) {
        wl_hints |= (ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT |
                     ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA);
    }

    return wl_hints;
}

static void
cog_im_context_wl_text_input_notify_content_type(CogIMContextWl *context)
{
    WebKitInputMethodContext *wk_context = WEBKIT_INPUT_METHOD_CONTEXT(context);
    WebKitInputPurpose purpose = webkit_input_method_context_get_input_purpose(wk_context);
    WebKitInputHints hints = webkit_input_method_context_get_input_hints(wk_context);

    zwp_text_input_v3_set_content_type (wl_text_input.text_input,
                                        wk_input_hints_to_wayland (hints, purpose),
                                        wk_input_purpose_to_wayland (purpose));
}

static void
cog_im_context_wl_text_input_notify_cursor_rectangle(CogIMContextWl *context)
{
    CogIMContextWlPrivate *priv = PRIV(context);

    zwp_text_input_v3_set_cursor_rectangle(wl_text_input.text_input,
                                           priv->cursor_rect.x,
                                           priv->cursor_rect.y,
                                           priv->cursor_rect.width,
                                           priv->cursor_rect.height);
}

static void
cog_im_context_wl_text_input_commit_state(CogIMContextWl *context)
{
    CogIMContextWlPrivate *priv = PRIV(context);

    wl_text_input.serial++;
    zwp_text_input_v3_commit(wl_text_input.text_input);
    priv->text_change_cause = ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD;
}

static void
cog_im_context_wl_text_input_enable(CogIMContextWl *context)
{
    zwp_text_input_v3_enable(wl_text_input.text_input);
    cog_im_context_wl_text_input_notify_surrounding(context);
    cog_im_context_wl_text_input_notify_content_type(context);
    cog_im_context_wl_text_input_notify_cursor_rectangle(context);
    cog_im_context_wl_text_input_commit_state(context);

    WebKitInputHints hints = webkit_input_method_context_get_input_hints(WEBKIT_INPUT_METHOD_CONTEXT(context));
    if (!(hints & WEBKIT_INPUT_HINT_INHIBIT_OSK)) {
        zwp_text_input_v3_enable(wl_text_input.text_input);
        cog_im_context_wl_text_input_commit_state(context);
    }
}

static void
cog_im_context_wl_text_input_disable(CogIMContextWl *context)
{
    zwp_text_input_v3_disable(wl_text_input.text_input);
    cog_im_context_wl_text_input_commit_state(context);
}

static void
cog_im_context_wl_preedit_apply(CogIMContextWl *context, uint32_t serial)
{
    CogIMContextWlPrivate *priv = PRIV(context);

    bool valid = wl_text_input.serial == serial;
    bool state_changed = (priv->current_preedit.text != NULL) != (priv->pending_preedit.text != NULL);
    if (valid && state_changed && !priv->current_preedit.text)
        g_signal_emit_by_name(context, "preedit-started");

    g_clear_pointer(&priv->current_preedit.text, g_free);
    priv->current_preedit = priv->pending_preedit;
    priv->pending_preedit.text = NULL;
    priv->pending_preedit.cursor_begin = priv->pending_preedit.cursor_end = 0;

    if (valid)
        g_signal_emit_by_name(context, "preedit-changed");

    if (valid && state_changed && !priv->current_preedit.text)
        g_signal_emit_by_name(context, "preedit-finished");
}

static void
cog_im_context_wl_commit_apply(CogIMContextWl *context, uint32_t serial)
{
    CogIMContextWlPrivate *priv = PRIV(context);

    bool valid = wl_text_input.serial == serial;
    if (valid && priv->pending_commit)
        g_signal_emit_by_name(context, "committed", priv->pending_commit);
    g_clear_pointer(&priv->pending_commit, g_free);
}

static void
cog_im_context_wl_delete_surrounding_text_apply(CogIMContextWl *context, uint32_t serial)
{
    CogIMContextWlPrivate *priv = PRIV(context);

    bool valid = wl_text_input.serial == serial;
    if (valid &&
        (priv->pending_surrounding_delete.before_length ||
         priv->pending_surrounding_delete.after_length)) {
        g_signal_emit_by_name (context,
                               "delete-surrounding",
                               -priv->pending_surrounding_delete.before_length,
                               priv->pending_surrounding_delete.before_length +
                               priv->pending_surrounding_delete.after_length);
    }
    priv->pending_surrounding_delete.before_length = 0;
    priv->pending_surrounding_delete.after_length = 0;
}

static void
text_input_enter (void *data,
                  struct zwp_text_input_v3 *text_input,
                  struct wl_surface *surface)
{
    wl_text_input.focused = true;

    if (wl_text_input.context)
        cog_im_context_wl_text_input_enable(COG_IM_CONTEXT_WL(wl_text_input.context));
}

static void
text_input_leave (void *data,
                  struct zwp_text_input_v3 *text_input,
                  struct wl_surface *surface)
{
    wl_text_input.focused = false;

    if (wl_text_input.context)
        cog_im_context_wl_text_input_disable(COG_IM_CONTEXT_WL(wl_text_input.context));
}

static void
text_input_preedit_string (void *data,
                           struct zwp_text_input_v3 *text_input,
                           const char *text,
                           int32_t cursor_begin,
                           int32_t cursor_end)
{
    if (!wl_text_input.context)
        return;

    Preedit *pending_preedit = &PRIV (wl_text_input.context)->pending_preedit;
    g_clear_pointer (&pending_preedit->text, g_free);
    pending_preedit->text = g_strdup (text);
    pending_preedit->cursor_begin = cursor_begin;
    pending_preedit->cursor_end = cursor_end;
}


static void
text_input_commit_string (void *data,
                          struct zwp_text_input_v3 *text_input,
                          const char *text)
{
    if (!wl_text_input.context)
        return;

    CogIMContextWlPrivate *priv = PRIV(wl_text_input.context);
    g_clear_pointer(&priv->pending_commit, g_free);
    priv->pending_commit = g_strdup (text);
}

static void
text_input_delete_surrounding_text (void *data,
                                    struct zwp_text_input_v3 *text_input,
                                    uint32_t before_length,
                                    uint32_t after_length)
{
    if (!wl_text_input.context)
        return;

    CogIMContextWlPrivate *priv = PRIV(wl_text_input.context);
    priv->pending_surrounding_delete.before_length = before_length;
    priv->pending_surrounding_delete.after_length = after_length;
}

static void
text_input_done (void *data,
                 struct zwp_text_input_v3 *text_input,
                 uint32_t serial)
{
    if (!wl_text_input.context)
        return;

    CogIMContextWl *context = COG_IM_CONTEXT_WL(wl_text_input.context);
    cog_im_context_wl_delete_surrounding_text_apply(context, serial);
    cog_im_context_wl_commit_apply(context, serial);
    cog_im_context_wl_preedit_apply(context, serial);
}

static const struct zwp_text_input_v3_listener text_input_listener = {
    .enter = text_input_enter,
    .leave = text_input_leave,
    .preedit_string = text_input_preedit_string,
    .commit_string = text_input_commit_string,
    .delete_surrounding_text = text_input_delete_surrounding_text,
    .done = text_input_done,
};

static void
cog_im_context_wl_finalize(GObject *object)
{
    CogIMContextWlPrivate *priv = PRIV(object);

    g_free(priv->pending_preedit.text);
    g_free(priv->current_preedit.text);
    g_free (priv->pending_commit);
    g_free(priv->surrounding.text);

    G_OBJECT_CLASS(cog_im_context_wl_parent_class)->finalize(object);
}

static void
cog_im_context_wl_get_preedit(WebKitInputMethodContext *context, char **text, GList **underlines, guint *cursor_offset)
{
    CogIMContextWlPrivate *priv = PRIV(context);

    if (text)
        *text = priv->current_preedit.text ? g_strdup (priv->current_preedit.text) : g_strdup ("");

    if (underlines) {
        *underlines = NULL;
        if (priv->current_preedit.cursor_begin != priv->current_preedit.cursor_end) {
            *underlines =
                g_list_prepend(*underlines, webkit_input_method_underline_new(priv->current_preedit.cursor_begin,
                                                                              priv->current_preedit.cursor_end));
        }
    }

    if (cursor_offset)
        *cursor_offset = priv->current_preedit.cursor_begin;
}

static void
cog_im_context_wl_notify_focus_in(WebKitInputMethodContext *context)
{
    if (wl_text_input.context == context)
        return;

    if (!wl_text_input.text_input)
        return;

    wl_text_input.context = context;
    if (wl_text_input.focused)
        cog_im_context_wl_text_input_enable(COG_IM_CONTEXT_WL(context));
}

static void
cog_im_context_wl_notify_focus_out(WebKitInputMethodContext *context)
{
    if (wl_text_input.context != context)
        return;

    if (wl_text_input.focused)
        cog_im_context_wl_text_input_disable(COG_IM_CONTEXT_WL(context));
    wl_text_input.context = NULL;
}

static void
cog_im_context_wl_notify_cursor_area(WebKitInputMethodContext *context, int x, int y, int width, int height)
{
    CogIMContextWlPrivate *priv = PRIV(context);

    if (priv->cursor_rect.x == x && priv->cursor_rect.y == y && priv->cursor_rect.width == width &&
        priv->cursor_rect.height == height)
        return;

    priv->cursor_rect.x = x;
    priv->cursor_rect.y = y;
    priv->cursor_rect.width = width;
    priv->cursor_rect.height = height;

    if (wl_text_input.context == context) {
        cog_im_context_wl_text_input_notify_cursor_rectangle(COG_IM_CONTEXT_WL(context));
        cog_im_context_wl_text_input_commit_state(COG_IM_CONTEXT_WL(context));
    }
}

static void
cog_im_context_wl_notify_surrounding(WebKitInputMethodContext *context,
                                     const char *text,
                                     guint length,
                                     guint cursor_index,
                                     guint selection_index)
{
    CogIMContextWlPrivate *priv = PRIV(context);

    g_clear_pointer(&priv->surrounding.text, g_free);
    priv->surrounding.text = g_strndup (text, length);
    priv->surrounding.cursor_index = cursor_index;
    priv->surrounding.anchor_index = selection_index;

    if (wl_text_input.context == context) {
        cog_im_context_wl_text_input_notify_surrounding(COG_IM_CONTEXT_WL(context));
        cog_im_context_wl_text_input_commit_state(COG_IM_CONTEXT_WL(context));
    }
}

static void
cog_im_context_wl_reset(WebKitInputMethodContext *context)
{
    if (wl_text_input.context != context)
        return;

    CogIMContextWlPrivate *priv = PRIV(context);
    priv->text_change_cause = ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER;
    cog_im_context_wl_text_input_notify_surrounding(COG_IM_CONTEXT_WL(context));
    cog_im_context_wl_text_input_commit_state(COG_IM_CONTEXT_WL(context));
}

static void
cog_im_context_wl_class_init(CogIMContextWlClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    gobject_class->finalize = cog_im_context_wl_finalize;

    WebKitInputMethodContextClass *im_context_class = WEBKIT_INPUT_METHOD_CONTEXT_CLASS (klass);
    im_context_class->get_preedit = cog_im_context_wl_get_preedit;
    im_context_class->notify_focus_in = cog_im_context_wl_notify_focus_in;
    im_context_class->notify_focus_out = cog_im_context_wl_notify_focus_out;
    im_context_class->notify_cursor_area = cog_im_context_wl_notify_cursor_area;
    im_context_class->notify_surrounding = cog_im_context_wl_notify_surrounding;
    im_context_class->reset = cog_im_context_wl_reset;
}

static void
cog_im_context_wl_content_type_changed(CogIMContextWl *context)
{
    if (wl_text_input.context != WEBKIT_INPUT_METHOD_CONTEXT(context))
        return;

    cog_im_context_wl_text_input_notify_content_type(context);
    cog_im_context_wl_text_input_commit_state(context);
}

static void
cog_im_context_wl_init(CogIMContextWl *context)
{
    g_signal_connect_swapped(context, "notify::input-purpose", G_CALLBACK(cog_im_context_wl_content_type_changed),
                             context);
    g_signal_connect_swapped(context, "notify::input-hints", G_CALLBACK(cog_im_context_wl_content_type_changed),
                             context);
}

void
cog_im_context_wl_set_text_input(struct zwp_text_input_v3 *text_input)
{
    g_clear_pointer(&wl_text_input.text_input, zwp_text_input_v3_destroy);
    wl_text_input.text_input = text_input;
    wl_text_input.serial = 0;
    if (wl_text_input.text_input)
        zwp_text_input_v3_add_listener(wl_text_input.text_input, &text_input_listener, NULL);
}

WebKitInputMethodContext *
cog_im_context_wl_new(void)
{
    return WEBKIT_INPUT_METHOD_CONTEXT(g_object_new(COG_TYPE_IM_CONTEXT_WL, NULL));
}
