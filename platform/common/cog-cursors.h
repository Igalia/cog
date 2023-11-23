/*
 * cog-cursors.h
 * Copyright (C) 2023 SUSE Software Solutions Germany GmbH
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

typedef enum {
    COG_CURSOR_TYPE_DEFAULT,
    COG_CURSOR_TYPE_HAND,
    COG_CURSOR_TYPE_TEXT,
} CogCursorType;

typedef const char *const *CogCursorNames;

CogCursorNames cog_cursors_get_names(CogCursorType type);
