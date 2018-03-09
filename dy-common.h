/*
 * dy-common.h
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#if !(defined(DY_INSIDE_DINGHY__) && DY_INSIDE_DINGHY__)
# error "Do not include this header directly, use <dinghy.h> instead"
#endif

#include <glib.h>

G_BEGIN_DECLS

char* dy_uri_guess_from_user_input (const char *uri_like,
                                    gboolean    is_cli_arg,
                                    GError    **error);

G_END_DECLS
