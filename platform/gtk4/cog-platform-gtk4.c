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

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720

/*
 * TODO
 * - fullscreen: if a video element switches to fullscreen it would be nice to also fullscreen the
     GTK window.
 * - multi-views
 * - settings UI?
 */

struct platform_window {
    WebKitWebView* web_view;

    GtkWidget* gtk_window;
    GtkWidget* gl_drawing_area;
    GtkWidget* back_button;
    GtkWidget* forward_button;
    GtkWidget* url_entry;

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC ext_glEGLImageTargetTexture2DOES;
    GLuint gl_program;
    GLint gl_texture_location;
    GLuint gl_texture;

    int width;
    int height;

    GdkModifierType key_modifiers;

    struct wpe_view_backend_exportable_fdo* exportable;
    WebKitWebViewBackend* view_backend;

    struct wpe_fdo_egl_exported_image* current_image;
    struct wpe_fdo_egl_exported_image* commited_image;
};

static struct platform_window win = {
    .gtk_window = NULL, .width = DEFAULT_WIDTH, .height = DEFAULT_HEIGHT
};

static const char s_vertex_shader[] = "#version 330\n"
                                      "attribute vec2 pos;\n"
                                      "attribute vec2 texture;\n"
                                      "varying vec2 v_texture;\n"
                                      "void main() {\n"
                                      "  v_texture = texture;\n"
                                      "  gl_Position = vec4(pos, 0, 1);\n"
                                      "}\n";

static const char s_fragment_shader[] = "#version 330\n"
                                        "precision mediump float;\n"
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
    if (gtk_gl_area_get_error(GTK_GL_AREA(window->gl_drawing_area)) != NULL)
        return false;

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

void resize(GtkGLArea* area, int width, int height, gpointer user_data)
{
    struct platform_window* win = user_data;
    win->width = width;
    win->height = height;
    wpe_view_backend_dispatch_set_size(
        wpe_view_backend_exportable_fdo_get_view_backend(win->exportable),
        win->width, win->height);
}

static void
quit_cb(GtkWidget* widget, gpointer data)
{
    if (g_application_get_default())
        g_application_quit(g_application_get_default());
}

static void
click_pressed(GtkGestureClick* gesture, int n_press, double x,
    double y, gpointer user_data)
{
    struct platform_window* win = user_data;
    struct wpe_input_pointer_event wpe_event;
    wpe_event.type = wpe_input_pointer_event_type_button;
    wpe_event.x = (int)x;
    wpe_event.y = (int)y;
    wpe_event.modifiers = wpe_input_pointer_modifier_button1;
    wpe_event.button = 1;
    wpe_event.state = 1;
    wpe_view_backend_dispatch_pointer_event(
        wpe_view_backend_exportable_fdo_get_view_backend(win->exportable),
        &wpe_event);
}

static void
click_released(GtkGestureClick* gesture, int n_press, double x,
    double y, gpointer user_data)
{
    struct platform_window* win = user_data;
    struct wpe_input_pointer_event wpe_event;
    wpe_event.type = wpe_input_pointer_event_type_button;
    wpe_event.x = (int)x;
    wpe_event.y = (int)y;
    wpe_event.modifiers = wpe_input_pointer_modifier_button1;
    wpe_event.button = 1;
    wpe_event.state = 0;
    wpe_view_backend_dispatch_pointer_event(
        wpe_view_backend_exportable_fdo_get_view_backend(win->exportable),
        &wpe_event);
}

static void
motion_cb(GtkEventControllerMotion* controller, double x, double y,
    gpointer user_data)
{
    struct platform_window* win = user_data;
    struct wpe_input_pointer_event wpe_event;
    wpe_event.type = wpe_input_pointer_event_type_motion;
    wpe_event.x = (int)x;
    wpe_event.y = (int)y;
    wpe_view_backend_dispatch_pointer_event(
        wpe_view_backend_exportable_fdo_get_view_backend(win->exportable),
        &wpe_event);
}

static gboolean
scroll_cb(GtkEventControllerScroll* controller, double dx,
    double dy, gpointer user_data)
{
    struct platform_window* win = user_data;
    struct wpe_input_axis_event axis_event;
    axis_event.type = wpe_input_axis_event_type_motion;
    struct wpe_input_axis_2d_event event2d = { axis_event, 0, 0 };
    if (dx) {
        axis_event.axis = 0;
        axis_event.value = event2d.x_axis = -dx * 100;
    } else {
        axis_event.axis = 1;
        axis_event.value = event2d.y_axis = -dy * 100;
    }
    event2d.base.type = wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth;
    wpe_view_backend_dispatch_axis_event(
        wpe_view_backend_exportable_fdo_get_view_backend(win->exportable),
        &event2d.base);
    return TRUE;
}

static gboolean
dispatch_key_event(struct platform_window* win, guint keycode, guint hardware_keycode, gboolean pressed, GdkModifierType state)
{
    struct wpe_input_keyboard_event wpe_event;
    wpe_event.key_code = keycode;
    wpe_event.hardware_key_code = hardware_keycode;
    wpe_event.pressed = pressed;

    uint32_t modifiers = 0;
    state |= win->key_modifiers;
    if (state & GDK_CONTROL_MASK)
        modifiers |= wpe_input_keyboard_modifier_control;
    if (state & GDK_ALT_MASK)
        modifiers |= wpe_input_keyboard_modifier_alt;
    if (state & GDK_SHIFT_MASK)
        modifiers |= wpe_input_keyboard_modifier_shift;

    wpe_event.modifiers = modifiers;
    wpe_view_backend_dispatch_keyboard_event(
        wpe_view_backend_exportable_fdo_get_view_backend(win->exportable),
        &wpe_event);
    return TRUE;
}

static gboolean
key_pressed_cb(GtkEventControllerKey* controller,
    guint keycode,
    guint hardware_keycode,
    GdkModifierType state,
    gpointer user_data)
{
    struct platform_window* win = user_data;
    return dispatch_key_event(win, keycode, hardware_keycode, TRUE, state);
}

static gboolean
key_released_cb(GtkEventControllerKey* controller,
    guint keycode,
    guint hardware_keycode,
    GdkModifierType state,
    gpointer user_data)
{
    struct platform_window* win = user_data;
    return dispatch_key_event(win, keycode, hardware_keycode, FALSE, state);
}

static gboolean
key_modifiers_cb(GtkEventControllerKey* controller,
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

static void
setup_window(struct platform_window* window)
{
    g_assert_nonnull(window);
    window->gtk_window = gtk_window_new();
    g_object_set(window->gtk_window, "default-width", DEFAULT_WIDTH,
        "default-height", DEFAULT_HEIGHT, NULL);
    g_signal_connect(window->gtk_window, "destroy", G_CALLBACK(quit_cb), NULL);

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

    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header_bar), title_box);
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header_bar), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(window->gtk_window), header_bar);

    window->gl_drawing_area = gtk_gl_area_new();
    gtk_widget_set_hexpand(window->gl_drawing_area, TRUE);
    gtk_widget_set_vexpand(window->gl_drawing_area, TRUE);
    gtk_widget_set_focus_on_click(window->gl_drawing_area, TRUE);
    g_object_set(window->gl_drawing_area, "use-es", TRUE, NULL);
    g_signal_connect(window->gl_drawing_area, "realize", G_CALLBACK(realize), window);
    g_signal_connect(window->gl_drawing_area, "render", G_CALLBACK(render), window);
    g_signal_connect(window->gl_drawing_area, "resize", G_CALLBACK(resize), window);

    GtkGesture* press = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(press), GDK_BUTTON_PRIMARY);
    g_signal_connect(press, "pressed", G_CALLBACK(click_pressed), window);
    g_signal_connect(press, "released", G_CALLBACK(click_released), window);
    gtk_widget_add_controller(window->gl_drawing_area, GTK_EVENT_CONTROLLER(press));

    GtkEventController* motion_controller = gtk_event_controller_motion_new();
    GtkEventControllerMotion* motion = GTK_EVENT_CONTROLLER_MOTION(motion_controller);
    g_signal_connect(motion, "motion", G_CALLBACK(motion_cb), window);
    gtk_widget_add_controller(window->gl_drawing_area, motion_controller);

    GtkEventController* scroll_controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    GtkEventControllerScroll* scroll = GTK_EVENT_CONTROLLER_SCROLL(scroll_controller);
    g_signal_connect(scroll, "scroll", G_CALLBACK(scroll_cb), window);
    gtk_widget_add_controller(window->gl_drawing_area, scroll_controller);

    GtkEventController* key_controller = gtk_event_controller_key_new();
    GtkEventControllerKey* key = GTK_EVENT_CONTROLLER_KEY(key_controller);
    g_signal_connect(key, "key-pressed", G_CALLBACK(key_pressed_cb), window);
    g_signal_connect(key, "key-released", G_CALLBACK(key_released_cb), window);
    g_signal_connect(key, "modifiers", G_CALLBACK(key_modifiers_cb), window);
    gtk_widget_add_controller(window->gtk_window, key_controller);

    // FIXME: Surely there is a simpler way to add shortcuts?
    GtkEventController* shortcut_controller = gtk_shortcut_controller_new();
    GtkShortcutController* shortcut = GTK_SHORTCUT_CONTROLLER(shortcut_controller);
    GtkShortcutAction* quit_action = gtk_callback_action_new(action_quit, NULL, NULL);
    GtkShortcutTrigger* quit_trigger = gtk_shortcut_trigger_parse_string("<Control>q");
    GtkShortcut* quit_shortcut = gtk_shortcut_new(quit_trigger, quit_action);
    gtk_shortcut_controller_add_shortcut(shortcut, quit_shortcut);

    GtkShortcutAction* activate_entry_action = gtk_callback_action_new(action_activate_entry, NULL, NULL);
    GtkShortcutTrigger* activate_entry_trigger = gtk_shortcut_trigger_parse_string("<Control>l");
    GtkShortcut* activate_entry_shortcut = gtk_shortcut_new(activate_entry_trigger, activate_entry_action);
    gtk_shortcut_controller_add_shortcut(shortcut, activate_entry_shortcut);

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

gboolean
cog_platform_plugin_setup(CogPlatform* platform,
    CogShell* shell G_GNUC_UNUSED,
    const char* params, GError** error)
{
    g_assert_nonnull(platform);

    wpe_loader_init("libWPEBackend-fdo-1.0.so");
    if (!gtk_init_check()) {
        g_set_error_literal(error, COG_PLATFORM_EGL_ERROR, 0,
            "GTK initialization failed");
        return FALSE;
    }

    setup_window(&win);
    setup_fdo_exportable(&win);
    return TRUE;
}

void cog_platform_plugin_teardown(CogPlatform* platform)
{
    g_assert_nonnull(platform);
}

WebKitWebViewBackend* cog_platform_plugin_get_view_backend(
    CogPlatform* platform, WebKitWebView* related_view, GError** error)
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
back_forward_changed(WebKitBackForwardList* back_forward_list,
    WebKitBackForwardListItem* item_added,
    gpointer items_removed, gpointer user_data)
{
    struct platform_window* win = user_data;
    gtk_widget_set_sensitive(win->back_button,
        webkit_web_view_can_go_back(win->web_view));
    gtk_widget_set_sensitive(win->forward_button,
        webkit_web_view_can_go_forward(win->web_view));
}

void cog_platform_plugin_init_web_view(CogPlatform* platform,
    WebKitWebView* view)
{
    g_signal_connect(view, "notify::title", G_CALLBACK(on_title_change), &win);
    g_signal_connect(view, "notify::uri", G_CALLBACK(on_uri_change), &win);
    g_signal_connect(view, "notify::estimated-load-progress",
        G_CALLBACK(on_load_progress), &win);
    g_signal_connect(webkit_web_view_get_back_forward_list(view), "changed",
        G_CALLBACK(back_forward_changed), &win);
    win.web_view = view;
}
