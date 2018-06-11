/*
 * cog-utils.h
 * Copyright (C) 2018 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#pragma once

#if !(defined(COG_INSIDE_COG__) && COG_INSIDE_COG__)
# error "Do not include this header directly, use <cog.h> instead"
#endif

#include <glib.h>

G_BEGIN_DECLS

typedef struct _GObjectClass GObjectClass;


char* cog_appid_to_dbus_object_path (const char *appid)
    G_GNUC_WARN_UNUSED_RESULT;

char* cog_uri_guess_from_user_input (const char *uri_like,
                                     gboolean    is_cli_arg,
                                     GError    **error);

GOptionEntry* cog_option_entries_from_class (GObjectClass *klass);

G_END_DECLS
