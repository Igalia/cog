/*
 * cursors.h
 * Copyright (C) 2023 SUSE Software Solutions Germany GmbH
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#include <stdlib.h>

enum cursor_type {
    CURSOR_LEFT_PTR,
    CURSOR_HAND,
    CURSOR_TEXT,
    N_CURSOR_TYPES,
};

static const char *cursor_names[N_CURSOR_TYPES][5] = {
    {"default", "left_ptr", NULL, NULL, NULL},
    {"pointer", "hand", "hand1", "pointing_hand", NULL},
    {"text", "xterm", "ibeam", NULL, NULL},
};
