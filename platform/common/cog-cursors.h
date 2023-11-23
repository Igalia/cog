/*
 * cog-cursors.h
 * Copyright (C) 2023 SUSE Software Solutions Germany GmbH
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

typedef struct _WebKitHitTestResult WebKitHitTestResult;

typedef enum {
    COG_CURSOR_TYPE_DEFAULT,
    COG_CURSOR_TYPE_HAND,
    COG_CURSOR_TYPE_TEXT,
} CogCursorType;

typedef const char *const *CogCursorNames;

CogCursorNames cog_cursors_get_names(CogCursorType type);
CogCursorType  cog_cursors_get_type_for_hit_test(WebKitHitTestResult *hit_test);

static inline CogCursorNames
cog_cursors_get_names_for_hit_test(WebKitHitTestResult *hit_test)
{
    return cog_cursors_get_names(cog_cursors_get_type_for_hit_test(hit_test));
}
