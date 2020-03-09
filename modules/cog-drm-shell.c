/*
 * cog-drm-shell.c
 * Copyright (C) 2020 Adrian Perez de Castro <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "../platform/pdrm.h"

#include <cog.h>
#include <inttypes.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>


#if 0
# define TRACE(fmt, ...) g_debug ("%s: " fmt, G_STRFUNC, __VA_ARGS__)
#else
# define TRACE(fmt, ...) ((void) 0)
#endif


#if !defined(EGL_EXT_platform_base)
typedef EGLDisplay (EGLAPIENTRYP PFNEGLGETPLATFORMDISPLAYEXTPROC) (EGLenum platform, void *native_display, const EGLint *attrib_list);
#endif

#if !defined(EGL_MESA_platform_gbm) || !defined(EGL_KHR_platform_gbm)
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif


G_DECLARE_FINAL_TYPE (CogDrmShell, cog_drm_shell, COG, DRM_SHELL, CogShell)
G_DECLARE_FINAL_TYPE (CogDrmView, cog_drm_view, COG, DRM_VIEW, CogView)


static gboolean s_support_checked = FALSE;
static PdrmDisplay *s_display = NULL;
G_LOCK_DEFINE_STATIC (s_globals);


struct _CogDrmShellClass {
    CogShellClass parent_class;
};

struct _CogDrmShell {
    CogShell parent;
};


struct _CogDrmViewClass {
    CogViewClass parent_class;
};

struct _CogDrmView {
    CogView parent;

    GHashTable         *buffers; /* (wl_resource*, PdrmBuffer*) */

    PdrmBuffer         *last_buffer;
    PdrmBuffer         *next_buffer;
    struct wl_resource *last_resource;
    struct wl_resource *next_resource;

    struct wpe_view_backend_exportable_fdo *exportable;
};


static gboolean
cog_drm_shell_initable_init (GInitable    *initable,
                             GCancellable *cancellable G_GNUC_UNUSED,
                             GError      **error)
{
    if (!wpe_loader_init ("libWPEBackend-fdo-1.0.so")) {
        g_set_error_literal (error,
                             G_IO_ERROR,
                             G_IO_ERROR_FAILED,
                             "Could not initialize WPE loader.");
        return FALSE;
    }

    wpe_fdo_initialize_for_egl_display (pdrm_display_get_egl_display (s_display));
    g_debug ("%s: WPE-FDO initialized.", G_STRFUNC);

    GMainContext *context = g_main_context_get_thread_default ();
    pdrm_display_attach_sources (s_display, context);

    g_debug ("%s: Done.", G_STRFUNC);

    return TRUE;
}


static void
cog_drm_shell_initable_iface_init (GInitableIface *iface)
{
    iface->init = cog_drm_shell_initable_init;
}


G_DEFINE_DYNAMIC_TYPE_EXTENDED (CogDrmShell, cog_drm_shell, COG_TYPE_SHELL, 0,
    g_io_extension_point_implement (COG_MODULES_SHELL_EXTENSION_POINT,
                                    g_define_type_id, "drm", 50);
    G_IMPLEMENT_INTERFACE_DYNAMIC (G_TYPE_INITABLE, cog_drm_shell_initable_iface_init))

G_DEFINE_DYNAMIC_TYPE (CogDrmView, cog_drm_view, COG_TYPE_VIEW)


static gboolean
cog_drm_shell_is_supported (void)
{
    gboolean result = FALSE;

    G_LOCK (s_globals);
    if (s_support_checked) {
        if (s_display)
            result = TRUE;
    } else {
        g_autoptr(GError) error = NULL;
        g_autoptr(PdrmDisplay) display = pdrm_display_open (NULL, &error);
        if (!display) {
            g_debug ("%s: %s", G_STRFUNC, error->message);
        } else {
            s_display = g_steal_pointer (&display);
            result = TRUE;
        }
        s_support_checked = TRUE;
    }
    G_UNLOCK (s_globals);

    return result;
}


static void
cog_drm_shell_class_init (CogDrmShellClass *klass)
{
    CogShellClass *shell_class = COG_SHELL_CLASS (klass);
    shell_class->is_supported = cog_drm_shell_is_supported;
    shell_class->get_view_class = cog_drm_view_get_type;
}


static void
cog_drm_shell_class_finalize (CogDrmShellClass *klass)
{
}


static void
cog_drm_shell_init (CogDrmShell *self)
{
}


static void
cog_drm_view_destroy_backend (void *userdata)
{
    CogDrmView *self = userdata;
    g_debug ("%s: Destroying backend for view @ %p", G_STRFUNC, self);
    g_clear_pointer (&self->exportable, wpe_view_backend_exportable_fdo_destroy);
}


static void
cog_drm_view_drop_last_buffer (CogDrmView *self)
{
    g_assert (self);

    if (!self->last_buffer)
        return;

    TRACE ("Freeing last committed resource %p, buffer %p.",
           self->last_resource, self->last_buffer);
    g_assert (self->last_resource);

    g_hash_table_remove (self->buffers, self->last_resource);
    g_clear_pointer (&self->last_buffer, pdrm_buffer_destroy);
    wpe_view_backend_exportable_fdo_dispatch_release_buffer (self->exportable,
                                                             self->last_resource);
    self->last_resource = NULL;
}


static inline void
cog_drm_view_on_buffer_committed (PdrmBuffer *buffer,
                                  void       *data)
{
    CogDrmView *self = data;
    g_assert (self->next_buffer == buffer);

    if (self->next_buffer != self->last_buffer) {
        cog_drm_view_drop_last_buffer (self);
        self->last_buffer = self->next_buffer;
        self->last_resource = self->next_resource;
    }

    self->next_buffer = NULL;
    self->next_resource = NULL;

    wpe_view_backend_exportable_fdo_dispatch_frame_complete (self->exportable);
}


static inline void
cog_drm_view_handle_buffer (CogDrmView         *self,
                            struct wl_resource *resource,
                            PdrmBuffer         *buffer)
{
    g_assert (self);
    g_assert (resource);
    g_assert (buffer);

    if (cog_view_get_focused ((CogView*) self)) {
        TRACE ("View focused, committing buffer %p.", self->next_buffer);

        self->next_resource = resource;
        self->next_buffer = buffer;

        pdrm_buffer_commit (self->next_buffer,
                            cog_drm_view_on_buffer_committed,
                            self);
    } else {
        TRACE ("View not focused, delaying buffer %p.", self->next_buffer);

        cog_drm_view_drop_last_buffer (self);
        self->last_resource = resource;
        self->last_buffer = buffer;

        wpe_view_backend_exportable_fdo_dispatch_frame_complete (self->exportable);
    }
}


static void
cog_drm_view_on_export_buffer_resource (void               *data,
                                        struct wl_resource *resource)
{
    CogDrmView *self = data;

    PdrmBuffer *buffer = NULL;
    if (self->last_resource == resource) {
        TRACE ("Resource %p is the same as the last used resource.", resource);
        buffer = self->last_buffer;
    } else {
        /* Search in hash table. */
        buffer = g_hash_table_lookup (self->buffers, resource);
        TRACE ("Looked up resource %p -> buffer %p.", resource, buffer);
    }

    if (!buffer) {
        buffer = pdrm_display_import_resource (s_display, resource);
        TRACE ("Imported resource %p -> buffer %p.", resource, buffer);
        g_hash_table_insert (self->buffers, resource, buffer);
    }

    if (!buffer) {
        g_warning ("%s: Failed to obtain a buffer from a wl_resource.", G_STRFUNC);
        return;
    }

    cog_drm_view_handle_buffer (self, resource, buffer);
}


static void
cog_drm_view_on_export_dmabuf_resource (void                                                   *data,
                                        struct wpe_view_backend_exportable_fdo_dmabuf_resource *resource)
{
    CogDrmView *self = data;

    PdrmBuffer *buffer = NULL;
    if (self->last_resource == resource->buffer_resource) {
        buffer = self->last_buffer;
        TRACE ("Resource %p is the same as the last used resource.",
               resource->buffer_resource);
    } else {
        buffer = g_hash_table_lookup (self->buffers, resource->buffer_resource);
        TRACE ("Looked up resource %p -> buffer %p.",
               resource->buffer_resource, buffer);
    }

    if (!buffer) {
        buffer = pdrm_display_import_dmabuf (s_display, resource);
        TRACE ("Imported resource %p -> buffer %p.",
               resource->buffer_resource, buffer);
        g_hash_table_insert (self->buffers, resource->buffer_resource, buffer);
    }

    if (!buffer) {
        g_warning ("%s: Failed to obtain a buffer from DMA-BUF.", G_STRFUNC);
        return;
    }

    cog_drm_view_handle_buffer (self, resource->buffer_resource, buffer);
}


static void
cog_drm_view_setup (CogView *view)
{
    CogDrmView *self = COG_DRM_VIEW (view);

    uint32_t width, height;
    pdrm_display_get_size (s_display, &width, &height);

    static const struct wpe_view_backend_exportable_fdo_client exportable_client = {
        .export_buffer_resource = cog_drm_view_on_export_buffer_resource,
        .export_dmabuf_resource = cog_drm_view_on_export_dmabuf_resource,
    };
    self->exportable = wpe_view_backend_exportable_fdo_create (&exportable_client,
                                                               self,
                                                               width,
                                                               height);
    WebKitWebViewBackend *backend =
        webkit_web_view_backend_new (wpe_view_backend_exportable_fdo_get_view_backend (self->exportable),
                                     cog_drm_view_destroy_backend,
                                     self);

    GValue backend_value = G_VALUE_INIT;
    g_value_take_boxed (g_value_init (&backend_value,
                                      WEBKIT_TYPE_WEB_VIEW_BACKEND),
                        backend);
    g_object_set_property (G_OBJECT (self), "backend", &backend_value);

    g_debug ("%s: created @ %p, backend @ %p (%" PRIu32 "x%" PRIu32 "), exportable @ %p.",
             G_STRFUNC, self, backend, width, height, self->exportable);
}


static void
cog_drm_view_on_notify_focused (CogDrmView *self)
{
    if (!cog_view_get_focused ((CogView*) self))
        return;

    if (!self->last_buffer) {
        g_debug ("%s: No last buffer to show, skipping update.", G_STRFUNC);
        return;
    }

    pdrm_buffer_commit (self->last_buffer,
                        cog_drm_view_on_buffer_committed,
                        self);
}


static void
cog_drm_view_constructed (GObject *self)
{
    g_debug ("%s: Enter.", G_STRFUNC);

    G_OBJECT_CLASS (cog_drm_view_parent_class)->constructed (self);

    /* After a view has been focused, window contents must be updated. */
    g_signal_connect_after (self, "notify::focused",
                            G_CALLBACK (cog_drm_view_on_notify_focused),
                            NULL);

    /*
     * Keep the "in-window" and "visible" synchronized with "focused":
     * in single window mode the focused view is the only one visible.
     */
    g_object_bind_property (self, "focused", self, "in-window",
                            G_BINDING_SYNC_CREATE | G_BINDING_DEFAULT);
    g_object_bind_property (self, "focused", self, "visible",
                            G_BINDING_SYNC_CREATE | G_BINDING_DEFAULT);

    g_debug ("%s: Done.", G_STRFUNC);
}


static void
cog_drm_view_dispose (GObject *object)
{
    g_debug ("%s: Enter.", G_STRFUNC);

    CogDrmView *self = COG_DRM_VIEW (object);

    g_clear_pointer (&self->last_buffer, pdrm_buffer_destroy);
    if (self->last_resource) {
        wpe_view_backend_exportable_fdo_dispatch_release_buffer (self->exportable,
                                                                 self->last_resource);
        self->last_resource = NULL;
    }

    G_OBJECT_CLASS (cog_drm_view_parent_class)->dispose (object);

    g_debug ("%s: Done.", G_STRFUNC);
}


static void
cog_drm_view_class_init (CogDrmViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->constructed = cog_drm_view_constructed;
    object_class->dispose = cog_drm_view_dispose;

    CogViewClass *view_class = COG_VIEW_CLASS (klass);
    view_class->setup = cog_drm_view_setup;
}


static void
cog_drm_view_class_finalize (CogDrmViewClass *klass)
{
}


static void
cog_drm_view_init (CogDrmView *self)
{
    self->buffers = g_hash_table_new (NULL, NULL);
    g_debug ("%s: Done.", G_STRFUNC);
}


G_MODULE_EXPORT
void
g_io_drm_shell_load (GIOModule *module)
{
    GTypeModule *type_module = G_TYPE_MODULE (module);
    cog_drm_shell_register_type (type_module);
    cog_drm_view_register_type (type_module);
}

G_MODULE_EXPORT
void
g_io_drm_shell_unload (GIOModule *module G_GNUC_UNUSED)
{
    G_LOCK (s_globals);
    g_clear_pointer (&s_display, pdrm_display_destroy);
    G_UNLOCK (s_globals);
}
