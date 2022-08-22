/*
 * cog-gamepad-manette.c
 * Copyright (C) 2022 Igalia S.L.
 *
 * Distributed under terms of the MIT license.
 */

#include "cog.h"

#include <libmanette.h>
#include <linux/input-event-codes.h>
#include <wpe/wpe.h>

typedef struct _Provider Provider;
typedef struct _Gamepad  Gamepad;

struct _Provider {
    struct wpe_gamepad_provider *wk_provider;
    ManetteMonitor              *monitor;
};

struct _Gamepad {
    struct wpe_gamepad *wk_gamepad;
    ManetteDevice      *device;
};

static void *
provider_create(struct wpe_gamepad_provider *wk_provider)
{
    Provider *provider = g_new0(Provider, 1);

    provider->wk_provider = wk_provider;
    return provider;
}

static void
provider_destroy(void *data)
{
    Provider *provider = data;

    if (!provider)
        return;

    g_clear_object(&provider->monitor);
    g_free(provider);
}

static void
add_device(Provider *provider, ManetteDevice *device)
{
    g_debug("gamepad: %s - connected", manette_device_get_name(device));

    uintptr_t gamepad_id = (uintptr_t) device;
    wpe_gamepad_provider_dispatch_gamepad_connected(provider->wk_provider, gamepad_id);
}

static gboolean
add_connected_devices(void *data)
{
    Provider                     *provider = data;
    g_autoptr(ManetteMonitorIter) iter = manette_monitor_iterate(provider->monitor);
    ManetteDevice                *device;

    while (manette_monitor_iter_next(iter, &device))
        add_device(provider, device);

    return G_SOURCE_REMOVE;
}

static void
on_device_connected(ManetteMonitor *monitor, ManetteDevice *device, void *data)
{
    add_device(data, device);
}

static void
on_device_disconnected(ManetteMonitor *monitor, ManetteDevice *device, void *data)
{
    Provider *provider = data;

    g_debug("gamepad: %s - disconnected", manette_device_get_name(device));

    uintptr_t gamepad_id = (uintptr_t) device;
    wpe_gamepad_provider_dispatch_gamepad_disconnected(provider->wk_provider, gamepad_id);
}

static void
provider_start(void *data)
{
    Provider *provider = data;

    /* double call to start? */
    if (provider->monitor)
        return;

    g_debug("gamepad: starting monitor");

    provider->monitor = manette_monitor_new();
    g_signal_connect(provider->monitor, "device-connected", G_CALLBACK(on_device_connected), provider);
    g_signal_connect(provider->monitor, "device-disconnected", G_CALLBACK(on_device_disconnected), provider);

    g_idle_add(add_connected_devices, provider);
}

static void
provider_stop(void *data)
{
    Provider *provider = data;

    if (!provider)
        return;

    g_debug("gamepad: stoping monitor");

    g_signal_handlers_disconnect_by_data(provider->monitor, provider);
}

static bool
to_standard_gamepad_axis(uint16_t axis, enum wpe_gamepad_axis *result)
{
    switch (axis) {
    case ABS_X:
        *result = WPE_GAMEPAD_AXIS_LEFT_STICK_X;
        break;
    case ABS_Y:
        *result = WPE_GAMEPAD_AXIS_LEFT_STICK_Y;
        break;
    case ABS_RX:
        *result = WPE_GAMEPAD_AXIS_RIGHT_STICK_X;
        break;
    case ABS_RY:
        *result = WPE_GAMEPAD_AXIS_RIGHT_STICK_Y;
        break;
    default:
        return false;
    }

    return true;
}

static void
on_absolute_axis(ManetteDevice *device, ManetteEvent *event, Gamepad *gamepad)
{
    uint16_t              axis;
    double                value;
    enum wpe_gamepad_axis std_axis;

    if (!manette_event_get_absolute(event, &axis, &value))
        return;

    if (to_standard_gamepad_axis(axis, &std_axis))
        wpe_gamepad_dispatch_axis_changed(gamepad->wk_gamepad, std_axis, value);
}

static bool
to_standard_gamepad_button(uint16_t manetteButton, enum wpe_gamepad_button *result)
{
    switch (manetteButton) {
    case BTN_SOUTH:
        *result = WPE_GAMEPAD_BUTTON_BOTTOM;
        break;
    case BTN_EAST:
        *result = WPE_GAMEPAD_BUTTON_RIGHT;
        break;
    case BTN_NORTH:
        *result = WPE_GAMEPAD_BUTTON_TOP;
        break;
    case BTN_WEST:
        *result = WPE_GAMEPAD_BUTTON_LEFT;
        break;
    case BTN_TL:
        *result = WPE_GAMEPAD_BUTTON_LEFT_SHOULDER;
        break;
    case BTN_TR:
        *result = WPE_GAMEPAD_BUTTON_RIGHT_SHOULDER;
        break;
    case BTN_TL2:
        *result = WPE_GAMEPAD_BUTTON_LEFT_TRIGGER;
        break;
    case BTN_TR2:
        *result = WPE_GAMEPAD_BUTTON_RIGHT_TRIGGER;
        break;
    case BTN_SELECT:
        *result = WPE_GAMEPAD_BUTTON_SELECT;
        break;
    case BTN_START:
        *result = WPE_GAMEPAD_BUTTON_START;
        break;
    case BTN_THUMBL:
        *result = WPE_GAMEPAD_BUTTON_LEFT_STICK;
        break;
    case BTN_THUMBR:
        *result = WPE_GAMEPAD_BUTTON_RIGHT_STICK;
        break;
    case BTN_DPAD_UP:
        *result = WPE_GAMEPAD_BUTTON_D_PAD_TOP;
        break;
    case BTN_DPAD_DOWN:
        *result = WPE_GAMEPAD_BUTTON_D_PAD_BOTTOM;
        break;
    case BTN_DPAD_LEFT:
        *result = WPE_GAMEPAD_BUTTON_D_PAD_LEFT;
        break;
    case BTN_DPAD_RIGHT:
        *result = WPE_GAMEPAD_BUTTON_D_PAD_RIGHT;
        break;
    case BTN_MODE:
        *result = WPE_GAMEPAD_BUTTON_CENTER;
        break;
    default:
        return false;
    }

    return true;
}

static inline void
dispatch_button_event(Gamepad *gamepad, ManetteEvent *event, bool pressed)
{
    uint16_t                button;
    enum wpe_gamepad_button std_button;

    if (!manette_event_get_button(event, &button))
        return;

    if (to_standard_gamepad_button(button, &std_button))
        wpe_gamepad_dispatch_button_changed(gamepad->wk_gamepad, std_button, pressed);
}

static void
on_button_press(ManetteDevice *device, ManetteEvent *event, Gamepad *gamepad)
{
    dispatch_button_event(gamepad, event, true);
}

static void
on_button_release(ManetteDevice *device, ManetteEvent *event, Gamepad *gamepad)
{
    dispatch_button_event(gamepad, event, false);
}

static void
on_gamepad_disconnected(ManetteDevice *device, Gamepad *gamepad)
{
    if (!gamepad)
        return;

    g_signal_handlers_disconnect_by_data(gamepad->device, gamepad);
}

static gpointer
gamepad_create(struct wpe_gamepad *wk_gamepad, struct wpe_gamepad_provider *wk_provider, uintptr_t gamepad_id)
{
    Provider *provider = wpe_gamepad_provider_get_backend(wk_provider);
    g_assert(provider);
    ManetteDevice *device = (ManetteDevice *) gamepad_id;
    g_assert(MANETTE_IS_DEVICE(device));

    Gamepad *gamepad = g_new0(Gamepad, 1);

    gamepad->wk_gamepad = wk_gamepad;
    gamepad->device = g_object_ref(device);

    g_signal_connect(device, "button-press-event", G_CALLBACK(on_button_press), gamepad);
    g_signal_connect(device, "button-release-event", G_CALLBACK(on_button_release), gamepad);
    g_signal_connect(device, "absolute-axis-event", G_CALLBACK(on_absolute_axis), gamepad);
    g_signal_connect(device, "disconnected", G_CALLBACK(on_gamepad_disconnected), gamepad);

    g_debug("gamepad: %s - created", manette_device_get_name(device));

    return gamepad;
}

static void
gamepad_destroy(void *data)
{
    Gamepad *gamepad = data;
    if (!gamepad)
        return;

    g_clear_object(&gamepad->device);
    g_free(gamepad);
}

static const char *
gamepad_get_id(void *data)
{
    Gamepad *gamepad = data;
    if (!(gamepad && gamepad->device))
        return "Unknown";

    return manette_device_get_name(gamepad->device);
}

const struct wpe_gamepad_provider_interface s_manette_provider_interface = {
    .create = provider_create,
    .destroy = provider_destroy,
    .start = provider_start,
    .stop = provider_stop,
};

const struct wpe_gamepad_interface s_manette_device_interface = {
    .create = gamepad_create,
    .destroy = gamepad_destroy,
    .get_id = gamepad_get_id,
};
