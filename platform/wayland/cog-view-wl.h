/*
 * cog-window-wl.h
 * Copyright (C) 2023 Pablo Saavedra <psaavedra@igalia.com>
 * Copyright (C) 2023 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef COG_PLATFORM_WL_VIEW_H
#define COG_PLATFORM_WL_VIEW_H

#include "../../core/cog.h"

#include "cog-platform-wl.h"
#include "cog-window-wl.h"

G_BEGIN_DECLS

typedef struct _CogWlPlatform CogWlPlatform;

/*
 * CogWlView type declaration.
 */

struct _CogWlView {
    CogView parent;

    CogWlPlatform *platform;

    CogViewStack *stack;

    struct wpe_view_backend_exportable_fdo *exportable;
    struct wpe_fdo_egl_exported_image      *image;

    struct wl_callback *frame_callback;

    bool    should_update_opaque_region;
    int32_t scale_factor;
};

G_DECLARE_FINAL_TYPE(CogWlView, cog_wl_view, COG, WL_VIEW, CogView)

/*
 * Method declarations.
 */

void cog_wl_view_register_type_exported(GTypeModule *type_module);

CogWlWindow *cog_wl_view_get_window(CogWlView *);
bool         cog_wl_view_does_image_match_win_size(CogWlView *);
void         cog_wl_view_fullscreen_image_ready(CogWlView *);
void         cog_wl_view_resize(CogWlView *view);
void         cog_wl_view_update_surface_contents(CogWlView *, struct wl_surface *);

G_END_DECLS

#endif /* !COG_PLATFORM_WL_VIEW_H */
