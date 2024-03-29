/*
 * cog-cursor-names.h
 * Copyright (C) 2023 Igalia S.L.
 * Copyright (C) 2023 SUSE Software Solutions Germany GmbH
 *
 * SPDX-License-Identifier: MIT
 */

#include "cog-cursors.h"
#include <wpe/webkit.h>

CogCursorNames
cog_cursors_get_names(CogCursorType type)
{
    static const char *const default_names[] = {"default", "left_ptr", NULL};
    static const char *const hand_names[] = {"pointer", "hand", "hand1", "pointing_hand", NULL};
    static const char *const text_names[] = {"text", "xterm", "ibeam", NULL, NULL};

    switch (type) {
    case COG_CURSOR_TYPE_DEFAULT:
        return default_names;
    case COG_CURSOR_TYPE_HAND:
        return hand_names;
    case COG_CURSOR_TYPE_TEXT:
        return text_names;
    default:
        g_assert_not_reached();
        return default_names;
    }
}

CogCursorType
cog_cursors_get_type_for_hit_test(WebKitHitTestResult *hit_test)
{
    g_assert(WEBKIT_IS_HIT_TEST_RESULT(hit_test));

    if (webkit_hit_test_result_context_is_link(hit_test))
        return COG_CURSOR_TYPE_HAND;

    if (webkit_hit_test_result_context_is_editable(hit_test) || webkit_hit_test_result_context_is_selection(hit_test))
        return COG_CURSOR_TYPE_TEXT;

    return COG_CURSOR_TYPE_DEFAULT;
}
