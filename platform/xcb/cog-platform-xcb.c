/*
 * cog-platform-xcb.c
 * Copyright (C) 2021 House Gordon Software Company LTD <kernel@housegordon.com>
 * Copyright (C) 2021 Igalia S.L
 *
 * Distributed under terms of the MIT license.
 */
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/xcb_image.h>
#include <xcb/xcb_keysyms.h>

#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>

#include <wayland-server.h>

#include "../../core/cog.h"


// Default values
#define DEFAULT_WIDTH  1024
#define DEFAULT_HEIGHT  768
#define DEFAULT_X        -1 // -1 = let the window manager position the window.
#define DEFAULT_Y        -1
#define DEFAULT_FPS 10
#define DEFAULT_SCROLL_DELTA (20)
#define DEFAULT_SCROLL_DIRECTION (1) /* change to -1 to reverse direction */
#define MAX_FPS 100 // quite arbitrary, but reasonable?


// Runtime configuration options,
// Set from command line.
static int s_x = DEFAULT_X;
static int s_y = DEFAULT_Y;
static int s_w = DEFAULT_WIDTH;
static int s_h = DEFAULT_HEIGHT;
static gboolean s_fullscreen = FALSE;
static int s_fps = DEFAULT_FPS ;
static int16_t s_scroll_delta = DEFAULT_SCROLL_DELTA ;
static int16_t s_scroll_direction = DEFAULT_SCROLL_DIRECTION ;
static gboolean s_ignore_keys = FALSE;
static gboolean s_ignore_mouse_buttons = FALSE;
static gboolean s_ignore_mouse_movement = FALSE;



struct Cog_XCB_Connection {
    xcb_connection_t *connection;
    xcb_screen_t *screen;

    xcb_key_symbols_t* keysyms;

    xcb_atom_t atom_wm_protocols;
    xcb_atom_t atom_wm_delete_window;
    xcb_atom_t atom_net_state;
    xcb_atom_t atom_net_state_fullscreen ;
};



struct Cog_XCB_Window {
    struct {
        xcb_window_t window;
        uint16_t window_width;
        uint16_t window_height;
        xcb_gcontext_t image_gc ;
        xcb_image_t *image;
    } xcb;


    struct {
        struct wpe_view_backend_exportable_fdo *exportable;
        struct wpe_view_backend *backend;
        gboolean frame_complete;
    } wpe;


    struct {
        uint8_t* buffer ;
        gsize    bufsize ;
        int32_t width;
        int32_t height;
        int32_t format;
        int32_t stride;
    } imgbuf;


    struct {
        guint tick_source;
        GSource* xcb_source;
        GPollFD  poll_fd;
    } glib;
};

static struct Cog_XCB_Connection s_conn;
static struct Cog_XCB_Window s_win;




/* Covnert XCB modifiers to WPE modifiers
    wpe constants from <wpe/input.h>,
    XCB constants from <xcb/xproto.h>
   Far from ideal implementation, but it avoids the need to link
   with xcb-keysyms (which is not often packaged by linux distributions)
   or requiring the X11/XKB extension (which is not always available, e.g. in VNC).
*/
static uint32_t
xcb_state_to_wpe_modifiers (uint16_t xcb_state)
{
    uint32_t out = 0;

    /* SHIFT, CONTROL, ALT/META keys */
    if ( xcb_state & XCB_MOD_MASK_SHIFT )
        out |= out | wpe_input_keyboard_modifier_shift;

    if ( xcb_state & XCB_MOD_MASK_CONTROL )
        out |= out | wpe_input_keyboard_modifier_control;

    if ( xcb_state & XCB_MOD_MASK_1 )
        out |= out | wpe_input_keyboard_modifier_alt;

    /* Mouse buttons */
    if ( xcb_state & XCB_BUTTON_MASK_1 )
        out |= out | wpe_input_pointer_modifier_button1;

    if ( xcb_state & XCB_BUTTON_MASK_2 )
        out |= out | wpe_input_pointer_modifier_button2;

    if ( xcb_state & XCB_BUTTON_MASK_3 )
        out |= out | wpe_input_pointer_modifier_button3;

    if ( xcb_state & XCB_BUTTON_MASK_4 )
        out |= out | wpe_input_pointer_modifier_button4;

    return out;
}



/*
 Convert XCB key press/release events to WPE events.
 NOTE:
 Due to the limitations of xcb_state_to_wpe_modifiers(),
 not all key combinations will be handled correctly.
*/
static void
xcb_handle_keys (const xcb_generic_event_t *generic_event, gboolean pressed)
{
    const xcb_key_press_event_t *event = (xcb_key_press_event_t *)generic_event;

    xcb_keysym_t keysym = xcb_key_symbols_get_keysym (s_conn.keysyms,
                                                      event->detail, 0);

    struct wpe_input_keyboard_event input_event = {
        .time = event->time,
        .key_code = keysym,
        .hardware_key_code = event->detail,
        .pressed = pressed,
        .modifiers = xcb_state_to_wpe_modifiers (event->state),
    };
    wpe_view_backend_dispatch_keyboard_event (s_win.wpe.backend, &input_event);
}

/*
  Convert XCB mouse wheel scroll events to
  WPE smooth scrolling events.
*/
static void
xcb_handle_axis (const xcb_button_press_event_t *event)
{
    g_assert (event->detail >= 4 && event->detail <= 7);
    const int btn = event->detail - 4;
    int16_t value ;
    int16_t axis;

    switch (btn)
        {
        case 0: // Scroll Wheel Vertical forward
            value = s_scroll_direction * s_scroll_delta ;
            axis = 2 ;
            break;
        case 1: // Scroll Wheel Vertical backwards
            value = -1 * s_scroll_direction * s_scroll_delta ;
            axis = 2 ;
            break;
        case 2: // Scroll Wheel Horizontal (Left?)
            value = s_scroll_direction * s_scroll_delta ;
            axis = 1 ;
            break;
        case 3: // Scroll Wheel Hprizontal (Right?)
            value = -1 * s_scroll_direction * s_scroll_delta ;
            axis = 1 ;
            break;
        }

    struct wpe_input_axis_event input_event = {
        .type = wpe_input_axis_event_type_motion_smooth,
        .time = event->time,
        .x = event->event_x,
        .y = event->event_y,
        .axis = axis,
        .value = value,
        .modifiers = xcb_state_to_wpe_modifiers (event->state),
    };

    wpe_view_backend_dispatch_axis_event (s_win.wpe.backend, &input_event);
}

/*
  Convert XCB mouse button press/release events
  to WPE events (including mouse wheel scroll events).
*/
static void
xcb_handle_buttons (const xcb_generic_event_t *generic_event, gboolean pressed)
{
    const xcb_button_press_event_t *event = (xcb_button_press_event_t *)generic_event;

    if (event->detail>=1 && event->detail<=3) {

        //Lleft/right/middle buttons
        struct wpe_input_pointer_event input_event = {
            .type = wpe_input_pointer_event_type_button,
            .time = event->time,
            .x = event->event_x,
            .y = event->event_y,
            .button = event->detail,
            .state = pressed,
            .modifiers = xcb_state_to_wpe_modifiers (event->state),
        };

        wpe_view_backend_dispatch_pointer_event (s_win.wpe.backend, &input_event);
    }
    else if (event->detail>=4 && event->detail<=7) {

        //Scroll wheel event
        xcb_handle_axis (event);
    }
}


/*
  Covnert XCB mouse/pointer movement events to WPE motion events.
*/
static void
xcb_handle_motion_event (const xcb_generic_event_t *generic_event)
{
    const xcb_motion_notify_event_t *event = (xcb_motion_notify_event_t *)generic_event;

    struct wpe_input_pointer_event input_event = {
        .type = wpe_input_pointer_event_type_motion,
        .time = event->time,
        .x = event->event_x,
        .y = event->event_y,
        .button = event->detail,
        .state = 0,
        .modifiers = xcb_state_to_wpe_modifiers(event->state),
    };

    wpe_view_backend_dispatch_pointer_event (s_win.wpe.backend, &input_event);
}


/*
  Repaint the content of the X11 window if we have a valid image buffer.
  The image buffer is created and updated when WPE calls the application's
  'shm_buffer' update function.

  X11 paiting is orthogonal to the WPE updates.
  It could happen that multiple 'xcb_repaint' events are called on the same
  image content (i.e. no updates from WPE),
  and also that multiple WPE updates change the content of the image buffer,
  but no 'xcb_repaint' events are made - meaning some intermediate WPE updates
  are lost.
*/
static void
xcb_repaint_window(const xcb_generic_event_t* generic_event)
{
    if (s_win.xcb.image) {
        xcb_image_put(s_conn.connection, s_win.xcb.window,
                      s_win.xcb.image_gc, s_win.xcb.image, 0, 0, 0);
        xcb_flush (s_conn.connection);
    }
}


/*
  Process X11/XCB resizing event and update the WPE backend
  with the new size.
  NOTE:
  XCB sends 'config-notify' event for several other cases (e.g. window movement),
  but this application ignores them.
*/
static void
xcb_handle_config_notify (const xcb_generic_event_t* generic_event)
{
    const xcb_configure_notify_event_t *event = (xcb_configure_notify_event_t *)generic_event;

    // bail if window size didn't change
    if (event->width == s_win.xcb.window_width &&
        event->height == s_win.xcb.window_height)
        return;

    s_win.xcb.window_width = event->width;
    s_win.xcb.window_height = event->height;

    wpe_view_backend_dispatch_set_size (s_win.wpe.backend,
                                        s_win.xcb.window_width,
                                        s_win.xcb.window_height);
}

/*
  Process X11 messages, wrapper as 'client message' by XCB.
  The only registered message in this application is
  'WM_DELETE_WINDOW' (user closed the window)
  for which we terminate the application.
*/
static void
xcb_handle_client_message (const xcb_generic_event_t* generic_event)
{
    const xcb_client_message_event_t *msg  = (xcb_client_message_event_t *)generic_event;

    if (msg->window != s_win.xcb.window) {
        g_warning("cli_msg: got a message, but not for our window, skipping");
        return;
    }

    if (msg->type != s_conn.atom_wm_protocols) {
        g_warning("cli_msg: got a message, but not WM_PROTOCOLS, skipping");
        return;
    }

    const xcb_atom_t at = msg->data.data32[0];

    if (at == s_conn.atom_wm_delete_window) {
        g_application_quit (g_application_get_default ());
        return;
    }
}

/*
  Print information about an XCB error notification.

  Not much we can do here, without adding more dependencies
  (e.g. xcb-utils-errors which is not commonly packaged
  by most linux distributions).

  While this is not very useful for the end-user, it can be useful
  for developers - if you see this message, a previous XCB call was invalid.
*/
static void
xcb_handle_error (const xcb_generic_event_t* generic_event)
{
  const xcb_generic_error_t *err = (xcb_generic_error_t*)generic_event;

  g_error("XCB error: error_code: 0x%x\t"
          "sequence: %d\t"
          "resource_id: 0x%x\t"
          "major_code: 0x%x\t"
          "minor_code: 0x%x\t"
          "full_sequence: 0x%x",
          err->error_code,
          err->sequence,
          err->resource_id,
          err->major_code,
          err->minor_code,
          err->full_sequence);
}

/*
  Handle X11/XCB events, and pass them on to the respective
  handler functions.

  NOTE:
  This application uses the glib framework - meaning the main event loop
  is elsewhere (see g_application_run() call in cog.c).

  This function uses the non-blocking 'xcb_poll_for_events',
  and is called by glib by registering a new GSourceFunc.
  All the pending XCB events are handled, then control returns to
  the glib framework.
  See glib_check_source() and glib_dispatch_source() functions below.
*/
static void
xcb_process_events (void)
{
  xcb_generic_event_t *event = NULL;

  while ((event = xcb_poll_for_event (s_conn.connection))) {

    const int event_code = event->response_type & 0x7f;

    switch (event_code)
      {
      case 0:
        xcb_handle_error(event);
        break;

      case XCB_CONFIGURE_NOTIFY:
        xcb_handle_config_notify(event);
        break;

      case XCB_EXPOSE:
        xcb_repaint_window(event);
        break;

      case XCB_CLIENT_MESSAGE:
        xcb_handle_client_message (event);
        break;

      case XCB_KEY_PRESS:
      case XCB_KEY_RELEASE:
        xcb_handle_keys (event, (event_code==XCB_KEY_PRESS));
        break;

      case XCB_BUTTON_PRESS:
      case XCB_BUTTON_RELEASE:
        xcb_handle_buttons (event, (event_code==XCB_BUTTON_PRESS));
        break;

      case XCB_MOTION_NOTIFY:
        xcb_handle_motion_event (event);
        break;

      default:
        break;
      }
    }
};

/*
  A helper function wrapper 'atom' + 'atom_reply', returning the ATOM value.
*/
static xcb_atom_t
get_atom (struct xcb_connection_t *connection, const char *name)
{
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom (connection, 0, strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply (connection, cookie, NULL);

    xcb_atom_t atom;
    if (reply) {
        atom = reply->atom;
        free(reply);
    } else
        atom = XCB_NONE;

    return atom;
}


/*
  Initialize the X11/XCB connection,
  create an X11 window, set the window properties (position, size, fullscreen).

  return FALSE on failure.
*/
static gboolean
xcb_init ()
{
    int err;

    s_conn.connection = xcb_connect(NULL,NULL);

    if ((err = xcb_connection_has_error (s_conn.connection))) {
        g_error("failed to connect to X11/XCB server, error code: 0x%0x", err); //FIXME: convert XCB_CONN to strings
        return FALSE;
    }

    // A poorman's alternative to requiring XKB extension.
    // this table will be used to translate hardware keycodes
    // to X11-keysyms. See xcb_handle_keys() above.
    s_conn.keysyms = xcb_key_symbols_alloc(s_conn.connection) ;


    const struct xcb_setup_t *setup = xcb_get_setup (s_conn.connection);
    g_assert( setup != NULL );
    s_conn.screen = xcb_setup_roots_iterator (setup).data;


    // Get few atom values, to be used later on
    s_conn.atom_wm_protocols = get_atom (s_conn.connection, "WM_PROTOCOLS");
    s_conn.atom_wm_delete_window = get_atom (s_conn.connection, "WM_DELETE_WINDOW");
    s_conn.atom_net_state = get_atom (s_conn.connection, "_NET_WM_STATE");
    s_conn.atom_net_state_fullscreen = get_atom (s_conn.connection, "_NET_WM_STATE_FULLSCREEN");



    s_win.xcb.window = xcb_generate_id (s_conn.connection);


    //
    // Create the X11 window
    //
    uint32_t window_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t window_values[2];
    window_values[0] = s_conn.screen->white_pixel; // value for XCB_CW_BACK_PIXEL

    // value for XCB_CW_EVENT_MASK
    window_values[1] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    if (!s_ignore_keys)
        window_values[1] |= XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE;
    if (!s_ignore_mouse_buttons)
        window_values[1] |= XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE;
    if (!s_ignore_mouse_movement)
        window_values[1] |= XCB_EVENT_MASK_POINTER_MOTION;

    s_win.xcb.window_width = s_w;
    s_win.xcb.window_height = s_h;

    xcb_create_window (s_conn.connection,
               XCB_COPY_FROM_PARENT,         // depth
               s_win.xcb.window,             // window-id
               s_conn.screen->root,          // parent window
               0, 0,                         // X, Y - Actual positioning is done below
               s_win.xcb.window_width, s_win.xcb.window_height, // W, H
               5,                            // Border Width
               XCB_WINDOW_CLASS_INPUT_OUTPUT,// Class
               s_conn.screen->root_visual,   // Visual
               window_mask, window_values);  // MASK & VALUES


    //
    // Register to receive WM_DELETE_WINDOW event (when user closes the window)
    //
    xcb_change_property (s_conn.connection,
         XCB_PROP_MODE_REPLACE,
         s_win.xcb.window,
         s_conn.atom_wm_protocols,
         XCB_ATOM_ATOM,
         32,
         1, &s_conn.atom_wm_delete_window);

    //
    // Set the window name
    //
    xcb_change_property (s_conn.connection,
         XCB_PROP_MODE_REPLACE,
         s_win.xcb.window,
         XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
         strlen("Cog"), "Cog");



    //
    // Create a graphic-context that will be used with out image buffer.
    // (see 'ttt_copy_shm_buffer' for image buffer handling).
    uint32_t gc_mask = XCB_GC_GRAPHICS_EXPOSURES;
    uint32_t gc_values[1] = { 0 } ;
    s_win.xcb.image_gc = xcb_generate_id (s_conn.connection);
    xcb_create_gc (s_conn.connection, s_win.xcb.image_gc, s_win.xcb.window, gc_mask, gc_values);



    // Full Screen - must happen BEFORE the window is mapped.
    // If you want to toggle AFTER, use "xcb_send_event"
    // See example at:
    // https://git.sailfishos.org/mer-core/libsdl/commit/c405df2801702e61c2c86e65f99f12bb0af6f11a#3921542f030c8ad1b52cbcc4739ee964f1695c81_848_848
    if (s_fullscreen)
        xcb_change_property (s_conn.connection,
             XCB_PROP_MODE_REPLACE,
             s_win.xcb.window,
             s_conn.atom_net_state,
             XCB_ATOM_ATOM,
             32,
             1,
             &s_conn.atom_net_state_fullscreen);



    xcb_map_window (s_conn.connection, s_win.xcb.window);


    //
    // Reposition the window - must happen AFTER the window is mapped.
    // see: https://mailman.videolan.org/pipermail/vlc-devel/2014-January/096507.html
    if (s_x != DEFAULT_X || s_y != DEFAULT_Y) {
        static uint32_t values[2];
        values[0] = s_x ;
        values[1] = s_y ;
        xcb_configure_window (s_conn.connection,
              s_win.xcb.window,
              XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
    }

    xcb_flush (s_conn.connection);

    return TRUE;
}

static void
xcb_clear (void)
{
    xcb_key_symbols_free (s_conn.keysyms);
    xcb_disconnect(s_conn.connection);
}






/*
  Given a wayland shm-buffer (representing a rendered screen,
  generated by WPE) - ensure that our internal image-buffer
  is big enough to store it. If not - re-allocate it,
  and re-create the XCB image object with the new size.
*/
static void
wpe_reserve_image_buffer (struct wl_shm_buffer *shm_buffer)
{
    int32_t width = wl_shm_buffer_get_width (shm_buffer);
    int32_t height = wl_shm_buffer_get_height (shm_buffer);
    int32_t stride = wl_shm_buffer_get_stride (shm_buffer);
    uint32_t format = wl_shm_buffer_get_format (shm_buffer);

    gsize need_size = stride * height ;

    if (need_size > s_win.imgbuf.bufsize) {
        s_win.imgbuf.buffer = g_realloc(s_win.imgbuf.buffer, need_size);
        s_win.imgbuf.bufsize = need_size ;
    }

    // If the XCB image is already allocated AND
    // is in the correct size and format, bail out.
    if ( (s_win.xcb.image != NULL) &&
         (width == s_win.imgbuf.width) &&
         (height == s_win.imgbuf.height) &&
         (stride == s_win.imgbuf.stride) &&
         (format == s_win.imgbuf.format))
        return;

    // Need to (re-)allocate the XCB image

    // First free the previous image, if any
    if (s_win.xcb.image) {
        xcb_image_destroy(s_win.xcb.image);
        s_win.xcb.image = NULL;
    }

    s_win.xcb.image = xcb_image_create_native(s_conn.connection,
              width,
              height,
              XCB_IMAGE_FORMAT_Z_PIXMAP,
              s_conn.screen->root_depth,
              NULL,
              0,
              s_win.imgbuf.buffer);

    s_win.imgbuf.width = width ;
    s_win.imgbuf.height = height ;
    s_win.imgbuf.format = format ;
    s_win.imgbuf.stride = stride ;
}


/*
  Given a wayland shm-buffer (representing a rendered screen,
  generated by WPE) - copy its content (e.g. raw RGBX bytes)
  to our own image-buffer, which is used as the data buffer
  for the XCB image.

  The next time the XCB image is painted on the XCB window,
  the new content will be shown.
*/
static void
wpe_copy_shm_buffer (struct wl_shm_buffer *shm_buffer)
{
    int32_t width = wl_shm_buffer_get_width (shm_buffer);
    int32_t height = wl_shm_buffer_get_height (shm_buffer);
    int32_t stride = wl_shm_buffer_get_stride (shm_buffer);
    uint32_t format = wl_shm_buffer_get_format (shm_buffer);

    //uint32_t bo_stride = 0;
    if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888) {
        g_warning("wpe_copy_shm_buffer: format is not recognized - skipping");
        return;
    }

    //The size of the image sent from WPE does not match the size of
    //the X11 window. Can commonly happen during a resize.
    //Skip updating the image.
    if (width != s_win.xcb.window_width || height != s_win.xcb.window_height)
        return;

    wpe_reserve_image_buffer (shm_buffer);

    wl_shm_buffer_begin_access (shm_buffer);

    uint8_t *src = wl_shm_buffer_get_data (shm_buffer);

    memmove(s_win.imgbuf.buffer, src, stride*height);

    wl_shm_buffer_end_access (shm_buffer);
}


/*
  This is the callback function used the WPE/FDO to notify the
  application that new rendered HTML content is available.

  This can be called rarely (on a simple static websites)
  or frequently (on very active website with lots of animations).

  To throttle the display speed, this function DOES NOT
  sends the "frame complete" ack back to WPE/FDO.
  instead, it sets the "frame_complete" flag to TRUE,
  and the "tick_source" will ACK the frame based on the FPS settings.
*/
static void
on_export_shm_buffer(void* data, struct wpe_fdo_shm_exported_buffer* exported_buffer)
{
    struct wl_shm_buffer *exported_shm_buffer = wpe_fdo_shm_exported_buffer_get_shm_buffer (exported_buffer);

    wpe_copy_shm_buffer(exported_shm_buffer);

    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(s_win.wpe.exportable, exported_buffer);

    //NOTE:
    //This draws the X11 window content immediately,
    //without waiting for an EXPOSE event - should "just work".
    xcb_repaint_window(NULL);

    s_win.wpe.frame_complete = TRUE ;
}



/*
  glib "GSource" functions - letting the glib framework
  poll the XCB Connection file handler for read/write events,
  and then handle them (by glib calling the 'glib_source_dispatch'
  function, and it calling the xcb_process_events() functions.

  See:
  https://developer.gnome.org/glib/stable/glib-The-Main-Event-Loop.html#glib-The-Main-Event-Loop.description
  https://developer.gnome.org/glib/stable/glib-The-Main-Event-Loop.html#GSourceFuncs
  https://developer.gnome.org/glib/stable/glib-The-Main-Event-Loop.html#g-source-add-poll
*/
static gboolean
glib_source_check (GSource *base)
{
    return !!s_win.glib.poll_fd.revents;
}

static gboolean
glib_source_dispatch (GSource *base, GSourceFunc callback, gpointer user_data)
{
    if (xcb_connection_has_error (s_conn.connection))
        return G_SOURCE_REMOVE;

    if (s_win.glib.poll_fd.revents & (G_IO_ERR | G_IO_HUP))
        return G_SOURCE_REMOVE;

    xcb_process_events ();
    s_win.glib.poll_fd.revents = 0;

    return G_SOURCE_CONTINUE;
}


/*
  This timer function is called by the glib framework.
  It ACKs a received 'shm_buffer' frame.
  See: on_export_shm_buffer() for details.
*/
static gboolean
glib_tick_callback(gpointer data)
{
    if (s_win.wpe.frame_complete) {
        s_win.wpe.frame_complete = FALSE ;
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(s_win.wpe.exportable);
    }
    return G_SOURCE_CONTINUE;
}


static gboolean
glib_init (void)
{
  static GSourceFuncs glib_source_funcs = {
     .check = glib_source_check,
     .dispatch = glib_source_dispatch,
  };

  //
  // Attach a new file-descriptor-polling source
  // to glib framework.
  s_win.glib.xcb_source = g_source_new (&glib_source_funcs, sizeof (GSource));

  s_win.glib.poll_fd.fd = xcb_get_file_descriptor (s_conn.connection);
  s_win.glib.poll_fd.events = G_IO_IN | G_IO_ERR | G_IO_HUP;
  s_win.glib.poll_fd.revents = 0;
  g_source_add_poll (s_win.glib.xcb_source, &s_win.glib.poll_fd);

  g_source_set_name (s_win.glib.xcb_source, "cog-xcb: xcb");
  g_source_set_can_recurse (s_win.glib.xcb_source, TRUE);
  g_source_attach (s_win.glib.xcb_source, g_main_context_get_thread_default ());


  //
  // Attach a timed source to the glib framework.
  //
  s_win.glib.tick_source = g_timeout_add(1000/s_fps,
                                         G_SOURCE_FUNC(glib_tick_callback), NULL);

  return TRUE;
}


static void
glib_clear (void)
{
    g_source_destroy (s_win.glib.xcb_source);
    g_clear_pointer (&s_win.glib.xcb_source, g_source_unref);

    g_source_remove(s_win.glib.tick_source);
}



/*
  Parse and validate a numeric value from a given string.
  The value must be a valid number, non-negative,
  non-zero (unless 'allow_zero' is true),
  must fit in an 'int', and not have any trailing characters.

  Returns TRUE if all the above conditions are met,
  with the numeric value stored in 'numval'.
*/
static gboolean
parse_option_numeric_value (const char* subopt_name,
                            const char* value,
                            gboolean allow_zero,
                            int* /*output*/ numval)
{
    if (value == NULL) {
        g_error("missing numeric value for sub-option '%s'", subopt_name);
        return FALSE;
    }

    char *endptr = NULL ;
    errno = 0 ;
    gint64 tmp = g_ascii_strtoll(value, &endptr, 10);
    if ( (tmp == G_MAXUINT64) || (tmp == G_MININT64) ||
         (tmp == 0 && errno == EINVAL) ||
         (endptr && *endptr != '\0')) {
        g_error("invalid value '%s' for sub-option '%s'", value, subopt_name);
        return FALSE;
    }

    // Zero and negative numbers are rejected (no option accepts them so far).
    if ( (tmp < 0) || ( tmp == 0 && !allow_zero) ) {
        g_error("invalid value '%ld' for sub-option '%s' - must be %s0",
                tmp, subopt_name, allow_zero?">=":">");
        return FALSE;
    }

    // Too-big values are rejected, so valid values will fit in an 'int')
    if (tmp > G_MAXINT) {
        g_error("invalid value '%ld' for sub-option '%s' - value too large", tmp, subopt_name);
        return FALSE;
    }

    g_assert_nonnull (numval);
    *numval = (int)tmp ;

    return TRUE;
}


/*
  Parse the option string passed to this platform module.
  The format should match the 'getsubopt(3)' function
  (think the 'mount -o [OPTIONS]' string).
*/
static gboolean
parse_option_string(const char* params)
{
    enum {
        FPS_OPT = 0,
        FULLSCREEN_OPT,
        WIDTH_OPT,
        HEIGHT_OPT,
        X_OPT,
        Y_OPT,
        SCROLL_DELTA_OPT,
        REV_SCROLL_DIRECTION_OPT,
        IGNORE_KEYS_OPT,
        IGNORE_MOUSE_BUTTONS_OPT,
        IGNORE_MOUSE_MOVEMENT_OPT
    };
    char *const token[] = {
        [FPS_OPT]   = "fps",
        [FULLSCREEN_OPT] = "fullscreen",
        [WIDTH_OPT] = "width",
        [HEIGHT_OPT] = "height",
        [X_OPT] = "x",
        [Y_OPT] = "y",
        [SCROLL_DELTA_OPT] = "scroll-delta",
        [REV_SCROLL_DIRECTION_OPT] = "rev-scroll-direction",
        [IGNORE_KEYS_OPT] = "ignore-keys",
        [IGNORE_MOUSE_BUTTONS_OPT] = "ignore-mouse-buttons",
        [IGNORE_MOUSE_MOVEMENT_OPT] = "ignore-mouse-movement",
        NULL
    };
    char *orig, *subopts;
    char *value;
    int opt;
    int numval ;
    gboolean fullscreen = FALSE;
    gboolean rev_scroll = FALSE;
    int scroll_delta = 0;
    int x=-1,y=-1,w=0,h=0;

    if (!params || *params=='\0')
        return TRUE;

    orig = subopts = g_strdup(params);

    while (*subopts != '\0') {
        opt = getsubopt(&subopts, token, &value);

        // These options require a numeric value
        if ( (opt == FPS_OPT) || (opt == WIDTH_OPT) || (opt==HEIGHT_OPT) ||
             (opt == X_OPT) || (opt == Y_OPT) || (opt== SCROLL_DELTA_OPT) ) {
            const gboolean allow_zero = ((opt == X_OPT) || (opt == Y_OPT));
            if (!parse_option_numeric_value (token[opt], value,
                                             allow_zero, &numval))
                goto fail;
        }

        switch (opt)
            {
            case FPS_OPT:
                if (numval > MAX_FPS) { // arbitrary ...
                    g_error("invalid FPS value '%d' - must be less than %d", numval, MAX_FPS);
                    goto fail;
                }
                s_fps = numval;
                break ;

            case FULLSCREEN_OPT:
                fullscreen = TRUE;
                break;

            case WIDTH_OPT:
                w = numval;
                break;

            case HEIGHT_OPT:
                h = numval;
                break;

            case X_OPT:
                x = numval;
                break;

            case Y_OPT:
                y = numval;
                break;

            case SCROLL_DELTA_OPT:
                scroll_delta = numval;
                break;

            case REV_SCROLL_DIRECTION_OPT:
                rev_scroll = TRUE;
                break;

            case IGNORE_KEYS_OPT:
                s_ignore_keys = TRUE;
                break;

            case IGNORE_MOUSE_BUTTONS_OPT:
                s_ignore_mouse_buttons = TRUE;
                break;

            case IGNORE_MOUSE_MOVEMENT_OPT:
                s_ignore_mouse_movement = TRUE;
                break;

            default:
                g_error("unknown sub-option found in '%s'", params);
                goto fail;
            }
    }

    //
    // Option parsing succeeded, now check validity of option combinations
    //


    // Fullscreen and specific coordinates are mutually exclusive
    if (fullscreen && ( (x>=0) || (y>=0) || w || h)) {
        g_error("fullscreen sub-option cannot be combined with x/y/w/h");
        goto fail;
    }

    // if one is set, the other is required.
    if ( ( (x>=0) && (y == -1) )
         ||
         ( (y>=0) && (x == -1) ) ) {
        g_error("please set BOTH x and y sub-options");
        goto fail;
    }

    // Override the defaults only for the specified options
    // (e.g. user can override Y and W but not X and H.
    if (x>=0)
        s_x = x;
    if (y>=0)
        s_y = y;
    if (w)
        s_w = w;
    if (h)
        s_h = h;

    if (fullscreen)
        s_fullscreen = TRUE;

    if (rev_scroll)
        s_scroll_direction = -1 ;

    if (scroll_delta)
        s_scroll_delta = scroll_delta;

    g_free (orig);
    return TRUE;

  fail:
    g_free (orig);
    return FALSE;
}


gboolean
cog_platform_plugin_setup (CogPlatform *platform,
                           CogShell    *shell G_GNUC_UNUSED,
                           const char  *params,
                           GError     **error)
{
    g_assert (platform);
    g_return_val_if_fail (COG_IS_SHELL (shell), FALSE);

    if (!parse_option_string (params)) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to parse XCB options");
        return FALSE;
    }

    if (!wpe_loader_init ("libWPEBackend-fdo-1.0.so")) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to set backend library name");
        return FALSE;
    }

    if (!xcb_init ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize XCB");
        return FALSE;
    }

    if (!glib_init ()) {
        g_set_error_literal (error,
                             COG_PLATFORM_WPE_ERROR,
                             COG_PLATFORM_WPE_ERROR_INIT,
                             "Failed to initialize GLib");
        return FALSE;
    }

    return TRUE;
}

void
cog_platform_plugin_teardown (CogPlatform *platform)
{
    g_assert (platform);

    xcb_clear ();
    glib_clear ();
}

WebKitWebViewBackend*
cog_platform_plugin_get_view_backend (CogPlatform   *platform,
                                      WebKitWebView *related_view,
                                      GError       **error)
{
    g_assert_nonnull(platform);

    wpe_fdo_initialize_shm();

    static const struct wpe_view_backend_exportable_fdo_client client = {
        .export_shm_buffer = on_export_shm_buffer,
    };

    s_win.wpe.exportable = wpe_view_backend_exportable_fdo_create(&client, &s_win,
                                                                  s_win.xcb.window_width,
                                                                  s_win.xcb.window_height);

    /* init WPE view backend */
    s_win.wpe.backend =
        wpe_view_backend_exportable_fdo_get_view_backend (s_win.wpe.exportable);
    g_assert (s_win.wpe.backend);

    WebKitWebViewBackend *wk_view_backend =
        webkit_web_view_backend_new (s_win.wpe.backend,
                                     (GDestroyNotify) wpe_view_backend_exportable_fdo_destroy,
                                     s_win.wpe.exportable);
    g_assert (wk_view_backend);
    return wk_view_backend;
}
