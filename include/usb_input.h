#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hid_gamepad.h"
#include "hid_keyboard.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Devices that can be plugged in at once. A keyboard that presents two interfaces takes two.
#define USB_INPUT_MAX_DEVICES 4

/// Gamepads that get a player number, which is as many as there are device slots
#define USB_INPUT_MAX_PLAYERS USB_INPUT_MAX_DEVICES

/// @brief What an interface was taken for when it was plugged in
typedef enum {
    USB_INPUT_KIND_NONE = 0,
    USB_INPUT_KIND_KEYBOARD,  ///< Boot keyboard, or any interface whose descriptor names keys
    USB_INPUT_KIND_MOUSE,     ///< Reports movement as a change rather than a position
    USB_INPUT_KIND_GAMEPAD,   ///< Recognised by hid-host's gamepad decoder
} usb_input_kind_t;

/// @brief What an event says
typedef enum {
    USB_INPUT_EVENT_CONNECTED = 0,  ///< An interface was taken into use; kind, vid and pid are set
    USB_INPUT_EVENT_DISCONNECTED,   ///< It went away; whatever it held down is no longer held
    USB_INPUT_EVENT_KEYBOARD,       ///< The keys a keyboard says are down, all of them
    USB_INPUT_EVENT_MOUSE,          ///< Movement since the previous report, and the buttons now
    USB_INPUT_EVENT_GAMEPAD,        ///< The state of a gamepad, all of it
} usb_input_event_type_t;

/// @brief One mouse report
///
/// Movement is a change and every report counts, so a caller adds them up rather than keeping
/// the last. The buttons are a state, so the newest report is the one that is true.
typedef struct {
    int32_t dx;
    int32_t dy;
    int32_t wheel;
    uint8_t buttons;  ///< Bit 0 left, bit 1 right, bit 2 middle, then whatever else the mouse has
} usb_input_mouse_t;

/// @brief One event, handed to the callback
typedef struct {
    usb_input_event_type_t type;
    uint8_t                device;  ///< Slot, stable for as long as the interface stays plugged in
    usb_input_kind_t       kind;
    /// Gamepads only: which player, from 0, in the order they were plugged in. A pad keeps its
    /// player while it stays plugged in, so unplugging player 0 does not promote player 1.
    uint8_t  player;
    uint16_t vid;  ///< Set on USB_INPUT_EVENT_CONNECTED
    uint16_t pid;  ///< Set on USB_INPUT_EVENT_CONNECTED
    union {
        hid_keyboard_state_t keyboard;  ///< USB_INPUT_EVENT_KEYBOARD
        usb_input_mouse_t    mouse;     ///< USB_INPUT_EVENT_MOUSE
        hid_gamepad_state_t  gamepad;   ///< USB_INPUT_EVENT_GAMEPAD
    };
} usb_input_event_t;

/// @brief Receives every event
///
/// Called on the component's tasks: reports and disconnects on the HID driver's, connects on the
/// component's own, and the disconnects usb_input_stop() sends on whichever task called it. So it
/// must be safe to call from more than one task, must return quickly, and must not call
/// usb_input_stop(). A mouse can report a thousand times a second; a callback that queues each
/// report rather than adding it up will drop movement when its queue fills.
typedef void (*usb_input_callback_t)(const usb_input_event_t* event, void* arg);

/// @brief How to start
typedef struct {
    usb_input_callback_t callback;
    void*                callback_arg;
    int                  core_id;        ///< Core for both tasks, or tskNO_AFFINITY
    unsigned             task_priority;  ///< The HID driver's task; devices are opened one below it
} usb_input_config_t;

#define USB_INPUT_CONFIG_DEFAULT() \
    { .callback = NULL, .callback_arg = NULL, .core_id = 0, .task_priority = 5 }

/// @brief Start the USB host library and the HID class driver, and take devices as they come
///
/// The port must already have power: on a board where the host port is switched, turn it on
/// before calling this. Devices plugged in before the call are found as well.
///
/// @return ESP_OK, or the error of whichever step failed, after undoing the steps before it
esp_err_t usb_input_start(const usb_input_config_t* config);

/// @brief Stop and take everything down again
///
/// Safe to call when start failed or was never called. Every device that was plugged in gets a
/// USB_INPUT_EVENT_DISCONNECTED first, so a caller can let go of what it held.
void usb_input_stop(void);

#ifdef __cplusplus
}
#endif
