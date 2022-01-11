/*
 * cog-platform-gtk4.c
 * Copyright (C) 2021 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#include <epoxy/egl.h>
#include <gtk/gtk.h>
#include <wpe/fdo-egl.h>
#include <wpe/fdo.h>

#include "../../core/cog.h"
#include "../common/egl-proc-address.h"
#include "cog-gtk-settings-dialog.h"

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720

#if defined(WPE_FDO_CHECK_VERSION)
#    define HAVE_FULLSCREEN_HANDLING WPE_FDO_CHECK_VERSION(1, 11, 1)
#else
#    define HAVE_FULLSCREEN_HANDLING 0
#endif

struct _CogGtk4PlatformClass {
    CogPlatformClass parent_class;
};

struct _CogGtk4Platform {
    CogPlatform parent;
};

G_DECLARE_FINAL_TYPE(CogGtk4Platform, cog_gtk4_platform, COG, GTK4_PLATFORM, CogPlatform)

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    CogGtk4Platform,
    cog_gtk4_platform,
    COG_TYPE_PLATFORM,
    0,
    g_io_extension_point_implement(COG_MODULES_PLATFORM_EXTENSION_POINT, g_define_type_id, "gtk4", 400);)

/*
 * TODO
 * - multi-views
 */

struct platform_window {
    WebKitWebView* web_view;

    GtkWidget* gtk_window;
    GtkWidget* gl_drawing_area;
    GtkWidget* back_button;
    GtkWidget* forward_button;
    GtkWidget* url_entry;
    GtkWidget* popover_menu;
    GtkWidget* settings_dialog;

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC ext_glEGLImageTargetTexture2DOES;
    GLuint gl_program;
    GLint gl_texture_location;
    GLuint gl_texture;

    int width;
    int height;
#if HAVE_FULLSCREEN_HANDLING
    bool is_fullscreen;
    bool waiting_fullscreen_notify;
#endif
    double device_scale_factor;

    GdkModifierType key_modifiers;

    struct wpe_view_backend_exportable_fdo* exportable;
    WebKitWebViewBackend* view_backend;

    struct wpe_fdo_egl_exported_image* current_image;
    struct wpe_fdo_egl_exported_image* commited_image;
};

static struct platform_window win = {
    .width = DEFAULT_WIDTH,
    .height = DEFAULT_HEIGHT,
    .device_scale_factor = 1,
};

static const char s_vertex_shader[] = "attribute vec2 pos;\n"
                                      "attribute vec2 texture;\n"
                                      "varying vec2 v_texture;\n"
                                      "void main() {\n"
                                      "  v_texture = texture;\n"
                                      "  gl_Position = vec4(pos, 0, 1);\n"
                                      "}\n";

static const char s_fragment_shader[] = "precision mediump float;\n"
                                        "uniform sampler2D u_tex;\n"
                                        "varying vec2 v_texture;\n"
                                        "void main() {\n"
                                        "  gl_FragColor = texture2D(u_tex, v_texture);\n"
                                        "}\n";

/*
 * The Shader loading code was originally authored by Adrian Perez de Castro.
 * TODO: Refactor it to common/ (see #275)
 */

typedef GLuint ShaderId;

static void
shader_id_destroy(ShaderId* shader_id)
{
    if (shader_id && *shader_id) {
        glDeleteShader(*shader_id);
        *shader_id = 0;
    }
}

static inline ShaderId
shader_id_steal(ShaderId* shader_id)
{
    g_assert(shader_id != NULL);
    ShaderId result = *shader_id;
    *shader_id = 0;
    return result;
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(ShaderId, shader_id_destroy)

static GLuint
load_shader(const char* source, GLenum kind, GError** error)
{
    g_assert(source != NULL);
    g_assert(kind == GL_VERTEX_SHADER || kind == GL_FRAGMENT_SHADER);

    GLenum err;
    g_auto(ShaderId) shader = glCreateShader(kind);

    glShaderSource(shader, 1, &source, NULL);
    if ((err = glGetError()) != GL_NO_ERROR) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, err,
            "Cannot set shader source");
        return 0;
    }

    glCompileShader(shader);
    if ((err = glGetError()) != GL_NO_ERROR) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, err,
            "Cannot compile shader");
        return 0;
    }

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE)
        return shader_id_steal(&shader);

    char buffer[4096] = {
        '\0',
    };
    glGetShaderInfoLog(shader, G_N_ELEMENTS(buffer), NULL, buffer);
    g_set_error(error, COG_PLATFORM_EGL_ERROR, 0, "Shader compilation: %s",
        buffer);
    return 0;
}

static bool
setup_shader(struct platform_window* window, GError** error)
{
    g_assert_nonnull(window);

    gtk_gl_area_make_current(GTK_GL_AREA(window->gl_drawing_area));
    g_debug("GL vendor: %s", glGetString(GL_VENDOR));
    g_debug("GL renderer: %s", glGetString(GL_RENDERER));
    g_debug("GL extensions: %s", glGetString(GL_EXTENSIONS));
    g_debug("GL version: %s", glGetString(GL_VERSION));
    g_debug("GLSL version: %s", glGetString(GL_SHADING_LANGUAGE_VERSION));

    /* if the GtkGLArea is in an error state we don't do anything */
    if (gtk_gl_area_get_error(GTK_GL_AREA(window->gl_drawing_area)) != NULL) {
        if (error)
            *error = g_error_copy(gtk_gl_area_get_error(GTK_GL_AREA(window->gl_drawing_area)));
        return false;
    }

    if (!(window->ext_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)load_egl_proc_address(
              "glEGLImageTargetTexture2DOES"))) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, eglGetError(),
            "Cannot obtain glEGLImageTargetTexture2DOES function");
        return false;
    }

    g_auto(ShaderId) vertex_shader = load_shader(s_vertex_shader, GL_VERTEX_SHADER, error);
    if (!vertex_shader)
        return false;

    g_auto(ShaderId) fragment_shader = load_shader(s_fragment_shader, GL_FRAGMENT_SHADER, error);
    if (!fragment_shader)
        return false;

    if (!(window->gl_program = glCreateProgram())) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, glGetError(),
            "Cannot create shader program");
        return false;
    }

    glAttachShader(window->gl_program, vertex_shader);
    glAttachShader(window->gl_program, fragment_shader);
    glBindAttribLocation(window->gl_program, 0, "pos");
    glBindAttribLocation(window->gl_program, 1, "texture");
    glLinkProgram(window->gl_program);

    GLuint err;
    if ((err = glGetError()) != GL_NO_ERROR) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, err,
            "Cannot link shader program");
        return false;
    }
    glUseProgram(window->gl_program);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if ((window->gl_texture_location = glGetUniformLocation(window->gl_program, "u_tex")) < 0) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, glGetError(),
            "Cannot obtain 'u_tex' uniform location");
        return false;
    }

    glGenTextures(1, &window->gl_texture);
    glBindTexture(GL_TEXTURE_2D, window->gl_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glActiveTexture(GL_TEXTURE0);
    glUniform1i(window->gl_texture_location, 0);

    /* Configuration for the FDO backend. */
    EGLDisplay display = eglGetCurrentDisplay();
    g_assert(display != EGL_NO_DISPLAY);

    EGLint egl_major = 0, egl_minor = 0;
    if (eglInitialize(display, &egl_major, &egl_minor) == EGL_FALSE) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, eglGetError(),
            "Cannot initialize EGL");
        return false;
    }

    g_debug("EGL %i.%i successfully initialized.", egl_major, egl_minor);
    wpe_fdo_initialize_for_egl_display(display);
    return true;
}

static void
realize(GtkWidget* widget, gpointer user_data)
{
    struct platform_window* win = user_data;
    g_autoptr(GError) error = NULL;
    if (!setup_shader(win, &error)) {
        g_warning("Shader setup failed: %s", error->message);
        g_application_quit(g_application_get_default());
    }
}

static gboolean
render(GtkGLArea* area, GdkGLContext* context, gpointer user_data)
{
    struct platform_window* win = user_data;
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(0, 0, win->width, win->height);

    if (win->commited_image)
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(
            win->exportable, win->commited_image);

    if (!win->current_image) {
        gtk_gl_area_queue_render(GTK_GL_AREA(win->gl_drawing_area));
        return TRUE;
    }

    glUseProgram(win->gl_program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, win->gl_texture);
    win->ext_glEGLImageTargetTexture2DOES(
        GL_TEXTURE_2D,
        wpe_fdo_egl_exported_image_get_egl_image(win->current_image));
    glUniform1i(win->gl_texture_location, 0);

    win->commited_image = win->current_image;

    static const GLfloat vertices[4][2] = {
        { -1.0, 1.0 },
        { 1.0, 1.0 },
        { -1.0, -1.0 },
        { 1.0, -1.0 },
    };

    static const GLfloat texturePos[4][2] = {
        { 0, 0 },
        { 1, 0 },
        { 0, 1 },
        { 1, 1 },
    };

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texturePos);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);

    glBindTexture(GL_TEXTURE_2D, 0);

    wpe_view_backend_exportable_fdo_dispatch_frame_complete(win->exportable);
    return TRUE;
}
static void
resize(GtkGLArea* area, int width, int height, gpointer user_data)
{
    struct platform_window* win = user_data;
    win->width = width;
    win->height = height;
    wpe_view_backend_dispatch_set_size(
        wpe_view_backend_exportable_fdo_get_view_backend(win->exportable),
        win->width, win->height);
}

#if HAVE_FULLSCREEN_HANDLING
static void
dispatch_wpe_fullscreen_event(struct platform_window* win)
{
    struct wpe_view_backend* backend = webkit_web_view_backend_get_wpe_backend(win->view_backend);
    if (win->is_fullscreen)
        wpe_view_backend_dispatch_did_enter_fullscreen(backend);
    else
        wpe_view_backend_dispatch_did_exit_fullscreen(backend);
}

static void
on_fullscreen_change(GtkWidget* window, GParamSpec* pspec, gpointer user_data)
{
    struct platform_window* win = user_data;
    bool was_fullscreen_requested_from_dom = win->waiting_fullscreen_notify;
    win->waiting_fullscreen_notify = false;
    win->is_fullscreen = gtk_window_is_fullscreen(GTK_WINDOW(window));

    if (!win->is_fullscreen && !was_fullscreen_requested_from_dom)
        wpe_view_backend_dispatch_request_exit_fullscreen(webkit_web_view_backend_get_wpe_backend(win->view_backend));
    else if (was_fullscreen_requested_from_dom)
        dispatch_wpe_fullscreen_event(win);
}
#endif

static void
on_quit(GtkWidget* widget, gpointer data)
{
    if (g_application_get_default())
        g_application_quit(g_application_get_default());
}

static void
on_click_pressed(GtkGestureClick* gesture, int n_press, double x,
    double y, gpointer user_data)
{
    struct platform_window* win = user_data;
    gtk_widget_grab_focus(win->gl_drawing_area);

    struct wpe_input_pointer_event wpe_event = {
        .type = wpe_input_pointer_event_type_button,
        .x = (int)x,
        .y = (int)y,
        .modifiers = wpe_input_pointer_modifier_button1,
        .button = 1,
        .state = 1,
    };
    wpe_view_backend_dispatch_pointer_event(
        wpe_view_backend_exportable_fdo_get_view_backend(win->exportable),
        &wpe_event);
}

static void
on_click_released(GtkGestureClick* gesture, int n_press, double x,
    double y, gpointer user_data)
{
    struct platform_window* win = user_data;
    struct wpe_input_pointer_event wpe_event = {
        .type = wpe_input_pointer_event_type_button,
        .x = (int)x,
        .y = (int)y,
        .modifiers = wpe_input_pointer_modifier_button1,
        .button = 1,
        .state = 0,
    };
    wpe_view_backend_dispatch_pointer_event(
        wpe_view_backend_exportable_fdo_get_view_backend(win->exportable),
        &wpe_event);
}

static void
on_motion(GtkEventControllerMotion* controller, double x, double y,
    gpointer user_data)
{
    struct platform_window* win = user_data;
    struct wpe_input_pointer_event wpe_event = {
        .type = wpe_input_pointer_event_type_motion,
        .x = (int)x,
        .y = (int)y,
    };
    wpe_view_backend_dispatch_pointer_event(
        wpe_view_backend_exportable_fdo_get_view_backend(win->exportable),
        &wpe_event);
}

static gboolean
on_scroll(GtkEventControllerScroll* controller, double dx,
    double dy, gpointer user_data)
{
    struct platform_window* win = user_data;
    struct wpe_input_axis_event axis_event = {
        .type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth,
        .axis = dx ? 0 : 1,
        .value = dx ? -dx * 100 : -dy * 100,
    };
    struct wpe_input_axis_2d_event event2d = {
        .base = axis_event,
        .x_axis = dx ? -dx * 100 : 0,
        .y_axis = dx ? 0 : -dy * 100,
    };
    wpe_view_backend_dispatch_axis_event(
        wpe_view_backend_exportable_fdo_get_view_backend(win->exportable),
        &event2d.base);
    return TRUE;
}

static gboolean
dispatch_key_event(struct platform_window* win, guint keycode, guint hardware_keycode, gboolean pressed, GdkModifierType state)
{
    uint32_t modifiers = 0;
    state |= win->key_modifiers;
    if (state & GDK_CONTROL_MASK)
        modifiers |= wpe_input_keyboard_modifier_control;
    if (state & GDK_ALT_MASK)
        modifiers |= wpe_input_keyboard_modifier_alt;
    if (state & GDK_SHIFT_MASK)
        modifiers |= wpe_input_keyboard_modifier_shift;

    struct wpe_input_keyboard_event wpe_event = {
        .key_code = keycode,
        .hardware_key_code = hardware_keycode,
        .pressed = pressed,
        .modifiers = modifiers,
    };

    wpe_view_backend_dispatch_keyboard_event(
        wpe_view_backend_exportable_fdo_get_view_backend(win->exportable),
        &wpe_event);
    return TRUE;
}

static gboolean
on_key_pressed(GtkEventControllerKey* controller,
    guint keycode,
    guint hardware_keycode,
    GdkModifierType state,
    gpointer user_data)
{
    struct platform_window* win = user_data;
    return dispatch_key_event(win, keycode, hardware_keycode, TRUE, state);
}

static gboolean
on_key_released(GtkEventControllerKey* controller,
    guint keycode,
    guint hardware_keycode,
    GdkModifierType state,
    gpointer user_data)
{
    struct platform_window* win = user_data;
    return dispatch_key_event(win, keycode, hardware_keycode, FALSE, state);
}

static gboolean
on_key_modifiers(GtkEventControllerKey* controller,
    GdkModifierType modifiers,
    gpointer user_data)
{
    struct platform_window* win = user_data;
    win->key_modifiers = modifiers;
    return TRUE;
}

static void
on_back_clicked(GtkWidget* widget, gpointer user_data)
{
    struct platform_window* win = user_data;
    webkit_web_view_go_back(win->web_view);
}

static void
on_forward_clicked(GtkWidget* widget, gpointer user_data)
{
    struct platform_window* win = user_data;
    webkit_web_view_go_forward(win->web_view);
}

static void
on_refresh_clicked(GtkWidget* widget, gpointer user_data)
{
    struct platform_window* win = user_data;
    webkit_web_view_reload(win->web_view);
}

static void
on_entry_activated(GtkWidget* widget, gpointer user_data)
{
    struct platform_window* win = user_data;
    GtkEntryBuffer* buffer = gtk_entry_get_buffer(GTK_ENTRY(win->url_entry));
    const gchar* uri = gtk_entry_buffer_get_text(buffer);
    webkit_web_view_load_uri(win->web_view, uri);
}

static gboolean
action_quit(GtkWidget* widget, GVariant* args,
    gpointer user_data)
{
    if (g_application_get_default())
        g_application_quit(g_application_get_default());

    return TRUE;
}

static gboolean
action_activate_entry(GtkWidget* widget, GVariant* args,
    gpointer user_data)
{
    struct platform_window* win = user_data;
    gtk_widget_grab_focus(win->url_entry);
    return TRUE;
}

#if HAVE_FULLSCREEN_HANDLING
static bool
on_dom_fullscreen_request(void* unused, bool fullscreen)
{
    if (win.waiting_fullscreen_notify)
        return false;

    if (fullscreen == win.is_fullscreen) {
        // Handle situations where DOM fullscreen requests are mixed with system fullscreen commands (e.g F11)
        dispatch_wpe_fullscreen_event(&win);
        return true;
    }

    win.waiting_fullscreen_notify = true;
    if (fullscreen)
        gtk_window_fullscreen(GTK_WINDOW(win.gtk_window));
    else
        gtk_window_unfullscreen(GTK_WINDOW(win.gtk_window));

    return true;
}
#endif

static gboolean
action_open_settings(GtkWidget* widget, GVariant* args,
    gpointer user_data)
{
    struct platform_window* win = user_data;

    if (win->settings_dialog) {
        gtk_window_present(GTK_WINDOW(win->settings_dialog));
        return TRUE;
    }

    win->settings_dialog = browser_settings_dialog_new(webkit_web_view_get_settings(win->web_view));
    gtk_window_set_transient_for(GTK_WINDOW(win->settings_dialog), GTK_WINDOW(win->gtk_window));
    g_object_add_weak_pointer(G_OBJECT(win->settings_dialog), (gpointer*)&win->settings_dialog);
    gtk_widget_show(win->settings_dialog);
    return TRUE;
}

static void
setup_window(struct platform_window* window)
{
    g_assert_nonnull(window);
    window->gtk_window = gtk_window_new();
    g_object_set(window->gtk_window, "default-width", DEFAULT_WIDTH,
        "default-height", DEFAULT_HEIGHT, NULL);
    g_signal_connect(window->gtk_window, "destroy", G_CALLBACK(on_quit), NULL);

    GtkWidget* header_bar = gtk_header_bar_new();
    GtkWidget* left_stack = gtk_stack_new();
    GtkWidget* buttons_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    window->back_button = gtk_button_new_from_icon_name("go-previous-symbolic");
    g_signal_connect(window->back_button, "clicked", G_CALLBACK(on_back_clicked), window);
    gtk_box_append(GTK_BOX(buttons_box), window->back_button);

    window->forward_button = gtk_button_new_from_icon_name("go-next-symbolic");
    g_signal_connect(window->forward_button, "clicked",
        G_CALLBACK(on_forward_clicked), window);
    gtk_box_append(GTK_BOX(buttons_box), window->forward_button);

    GtkWidget* refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), window);
    gtk_box_append(GTK_BOX(buttons_box), refresh_button);

    gtk_stack_add_named(GTK_STACK(left_stack), buttons_box, "buttons");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header_bar), left_stack);

    GtkWidget* title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(title_box, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(title_box, TRUE);

    window->url_entry = gtk_entry_new();
    g_signal_connect(window->url_entry, "activate", G_CALLBACK(on_entry_activated), window);
    gtk_widget_set_parent(window->url_entry, title_box);

    GtkWidget* right_stack = gtk_stack_new();

    g_autoptr(GSimpleActionGroup) action_group = g_simple_action_group_new();
    g_autoptr(GMenu) menu = g_menu_new();

    g_autoptr(GSimpleAction) action = g_simple_action_new("open-settings", NULL);
    g_signal_connect(action, "activate", G_CALLBACK(action_open_settings), window);
    g_action_map_add_action(G_ACTION_MAP(action_group), G_ACTION(action));
    g_autoptr(GMenuItem) menuItem = g_menu_item_new("Settings", "win.open-settings");
    g_menu_append_item(menu, menuItem);

    window->popover_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_widget_insert_action_group(window->popover_menu, "win", G_ACTION_GROUP(action_group));

    GtkWidget* right_buttons_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget* button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "open-menu-symbolic");
    gtk_box_append(GTK_BOX(right_buttons_box), button);
    gtk_stack_add_named(GTK_STACK(right_stack), right_buttons_box, "right_buttons");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), window->popover_menu);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header_bar), right_stack);

    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header_bar), title_box);
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header_bar), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(window->gtk_window), header_bar);

    window->gl_drawing_area = gtk_gl_area_new();
    gtk_widget_set_hexpand(window->gl_drawing_area, TRUE);
    gtk_widget_set_vexpand(window->gl_drawing_area, TRUE);
    gtk_widget_set_can_focus(window->gl_drawing_area, TRUE);
    gtk_widget_set_sensitive(window->gl_drawing_area, TRUE);
    gtk_widget_set_focusable(window->gl_drawing_area, TRUE);
    gtk_widget_set_focus_on_click(window->gl_drawing_area, TRUE);
    g_object_set(window->gl_drawing_area, "use-es", TRUE, NULL);
    g_signal_connect(window->gl_drawing_area, "realize", G_CALLBACK(realize), window);
    g_signal_connect(window->gl_drawing_area, "render", G_CALLBACK(render), window);
    g_signal_connect(window->gl_drawing_area, "resize", G_CALLBACK(resize), window);

#if HAVE_FULLSCREEN_HANDLING
    g_signal_connect(window->gtk_window, "notify::fullscreened", G_CALLBACK(on_fullscreen_change), window);
#endif

    GtkGesture* press = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(press), GDK_BUTTON_PRIMARY);
    g_signal_connect(press, "pressed", G_CALLBACK(on_click_pressed), window);
    g_signal_connect(press, "released", G_CALLBACK(on_click_released), window);
    gtk_widget_add_controller(window->gl_drawing_area, GTK_EVENT_CONTROLLER(press));

    GtkEventController* motion_controller = gtk_event_controller_motion_new();
    GtkEventControllerMotion* motion = GTK_EVENT_CONTROLLER_MOTION(motion_controller);
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), window);
    gtk_widget_add_controller(window->gl_drawing_area, motion_controller);

    GtkEventController* scroll_controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    GtkEventControllerScroll* scroll = GTK_EVENT_CONTROLLER_SCROLL(scroll_controller);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), window);
    gtk_widget_add_controller(window->gl_drawing_area, scroll_controller);

    GtkEventController* key_controller = gtk_event_controller_key_new();
    GtkEventControllerKey* key = GTK_EVENT_CONTROLLER_KEY(key_controller);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), window);
    g_signal_connect(key, "key-released", G_CALLBACK(on_key_released), window);
    g_signal_connect(key, "modifiers", G_CALLBACK(on_key_modifiers), window);
    gtk_widget_add_controller(window->gtk_window, key_controller);

    // FIXME: Surely there is a simpler way to add shortcuts?
    GtkEventController* shortcut_controller = gtk_shortcut_controller_new();
    GtkShortcutController* shortcut = GTK_SHORTCUT_CONTROLLER(shortcut_controller);
    GtkShortcutAction* quit_action = gtk_callback_action_new(action_quit, NULL, NULL);
    GtkShortcutTrigger* quit_trigger = gtk_shortcut_trigger_parse_string("<Control>q");
    GtkShortcut* quit_shortcut = gtk_shortcut_new(quit_trigger, quit_action);
    gtk_shortcut_controller_add_shortcut(shortcut, quit_shortcut);

    GtkShortcutAction* activate_entry_action = gtk_callback_action_new(action_activate_entry, window, NULL);
    GtkShortcutTrigger* activate_entry_trigger = gtk_shortcut_trigger_parse_string("<Control>l");
    GtkShortcut* activate_entry_shortcut = gtk_shortcut_new(activate_entry_trigger, activate_entry_action);
    gtk_shortcut_controller_add_shortcut(shortcut, activate_entry_shortcut);

    GtkShortcutAction* open_settings_action = gtk_callback_action_new(action_open_settings, window, NULL);
    GtkShortcutTrigger* open_settings_trigger = gtk_shortcut_trigger_parse_string("<Control>s");
    GtkShortcut* open_settings_shortcut = gtk_shortcut_new(open_settings_trigger, open_settings_action);
    gtk_shortcut_controller_add_shortcut(shortcut, open_settings_shortcut);

    gtk_widget_add_controller(window->gtk_window, shortcut_controller);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, FALSE);
    gtk_window_set_child(GTK_WINDOW(window->gtk_window), box);
    gtk_box_append(GTK_BOX(box), window->gl_drawing_area);

    gtk_widget_show(window->gtk_window);
}

static void
on_export_egl_image(void* userdata,
    struct wpe_fdo_egl_exported_image* image)
{
    struct platform_window* window = userdata;

    window->current_image = image;
    gtk_gl_area_queue_render(GTK_GL_AREA(window->gl_drawing_area));
}

static void
setup_fdo_exportable(struct platform_window* window)
{
    g_assert_nonnull(window);

    static const struct wpe_view_backend_exportable_fdo_egl_client client = {
        .export_fdo_egl_image = on_export_egl_image,
    };

    window->exportable = wpe_view_backend_exportable_fdo_egl_create(
        &client, window, (uint32_t)DEFAULT_WIDTH, (uint32_t)DEFAULT_HEIGHT);
    g_assert_nonnull(window->exportable);

    window->view_backend = webkit_web_view_backend_new(
        wpe_view_backend_exportable_fdo_get_view_backend(window->exportable),
        (GDestroyNotify)wpe_view_backend_exportable_fdo_destroy,
        window->exportable);
    g_assert_nonnull(window->view_backend);
}

static void
shell_device_factor_changed(CogShell *shell, GParamSpec *param_spec, gpointer data)
{

    struct platform_window *window = (struct platform_window *) data;
    window->device_scale_factor = cog_shell_get_device_scale_factor(shell);
}

static gboolean
cog_gtk4_platform_setup(CogPlatform *platform, CogShell *shell, const char *params, GError **error)
{
    g_assert_nonnull(platform);

    wpe_loader_init("libWPEBackend-fdo-1.0.so");
    if (!gtk_init_check()) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, 0,
            "GTK initialization failed");
        return FALSE;
    }

    g_signal_connect(shell, "notify::device-scale-factor", G_CALLBACK(shell_device_factor_changed), &win);

    setup_window(&win);
    setup_fdo_exportable(&win);

#if HAVE_FULLSCREEN_HANDLING
    wpe_view_backend_set_fullscreen_handler(webkit_web_view_backend_get_wpe_backend(win.view_backend),
                                            on_dom_fullscreen_request, NULL);
#endif

    return TRUE;
}

static WebKitWebViewBackend*
cog_gtk4_platform_get_view_backend(CogPlatform* platform, WebKitWebView* related_view, GError** error)
{
    g_assert_nonnull(platform);
    g_assert_nonnull(win.view_backend);
    return win.view_backend;
}

static void
on_title_change(WebKitWebView* view, GParamSpec* pspec,
    gpointer user_data)
{
    struct platform_window* win = user_data;
    g_autofree gchar* title;
    g_object_get(view, "title", &title, NULL);
    g_autofree gchar* win_title = g_strdup_printf("Cog - %s", title);
    gtk_window_set_title(GTK_WINDOW(win->gtk_window), win_title);
}

static void
on_uri_change(WebKitWebView* view, GParamSpec* pspec,
    gpointer user_data)
{
    struct platform_window* win = user_data;
    const char* uri = webkit_web_view_get_uri(view);
    GtkEntryBuffer* buffer = gtk_entry_get_buffer(GTK_ENTRY(win->url_entry));
    gtk_entry_buffer_set_text(buffer, uri, strlen(uri));
}

static void
on_load_progress(WebKitWebView* view, GParamSpec* pspec,
    gpointer user_data)
{
    struct platform_window* win = user_data;
    gdouble progress;
    g_object_get(view, "estimated-load-progress", &progress, NULL);
    gtk_entry_set_progress_fraction(GTK_ENTRY(win->url_entry),
        progress < 1 ? progress : 0);
}

static void
on_back_forward_changed(WebKitBackForwardList* back_forward_list,
    WebKitBackForwardListItem* item_added,
    gpointer items_removed, gpointer user_data)
{
    struct platform_window* win = user_data;
    gtk_widget_set_sensitive(win->back_button,
        webkit_web_view_can_go_back(win->web_view));
    gtk_widget_set_sensitive(win->forward_button,
        webkit_web_view_can_go_forward(win->web_view));
}

static void
cog_gtk4_platform_init_web_view(CogPlatform* platform, WebKitWebView* view)
{
    g_signal_connect(view, "notify::title", G_CALLBACK(on_title_change), &win);
    g_signal_connect(view, "notify::uri", G_CALLBACK(on_uri_change), &win);
    g_signal_connect(view, "notify::estimated-load-progress",
        G_CALLBACK(on_load_progress), &win);
    g_signal_connect(webkit_web_view_get_back_forward_list(view), "changed",
        G_CALLBACK(on_back_forward_changed), &win);
    win.web_view = view;

    wpe_view_backend_dispatch_set_device_scale_factor(wpe_view_backend_exportable_fdo_get_view_backend(win.exportable),
                                                      win.device_scale_factor);
}

static void*
check_supported(void* data G_GNUC_UNUSED)
{
    return GINT_TO_POINTER(gtk_init_check());
}

static gboolean
cog_gtk4_platform_is_supported(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, check_supported, NULL);
    return GPOINTER_TO_INT(once.retval);
}

static void
cog_gtk4_platform_class_init(CogGtk4PlatformClass* klass)
{
    CogPlatformClass* platform_class = COG_PLATFORM_CLASS(klass);
    platform_class->is_supported = cog_gtk4_platform_is_supported;
    platform_class->setup = cog_gtk4_platform_setup;
    platform_class->get_view_backend = cog_gtk4_platform_get_view_backend;
    platform_class->init_web_view = cog_gtk4_platform_init_web_view;
}

static void
cog_gtk4_platform_class_finalize(CogGtk4PlatformClass* klass)
{
}

static void
cog_gtk4_platform_init(CogGtk4Platform* self)
{
}

G_MODULE_EXPORT void
g_io_cogplatform_gtk4_load(GIOModule* module)
{
    GTypeModule* type_module = G_TYPE_MODULE(module);
    cog_gtk4_platform_register_type(type_module);
}

G_MODULE_EXPORT void
g_io_cogplatform_gtk4_unload(GIOModule* module G_GNUC_UNUSED)
{
}
