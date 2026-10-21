# usb-input

USB keyboards, mice and gamepads as input, for ESP-IDF on chips with a USB host port (ESP32-P4,
ESP32-S3, ESP32-S2).

It runs the ESP-IDF USB host library and its HID class driver, works out what each plugged-in
interface is, decodes its reports with [hid-host](https://github.com/badgeteam/esp32-component-hid-host),
and hands the result to one callback. What a key or a button means is left to the app.

- **Keyboards**: the boot keyboard interface in boot protocol, and any other interface whose report
  descriptor names keys, which is where a keyboard reports more than six keys at once. The event
  carries every key that is down, as a set of usages.
- **Mice**: anything that reports movement as a change. The event carries the movement since the
  previous report, the wheel and the buttons.
- **Gamepads**: whatever hid-host's gamepad decoder recognises, including the pads that need a nudge
  before they report. Each gets a player number in the order they were plugged in, and keeps it
  while it stays plugged in.
- Up to four interfaces at once. A keyboard with two interfaces takes two.

## Use

```yaml
dependencies:
  badgeteam/usb-input: "^0.1.0"
```

```c
#include "usb_input.h"

static void on_input(const usb_input_event_t* event, void* arg) {
    switch (event->type) {
        case USB_INPUT_EVENT_KEYBOARD:
            // event->keyboard: hid_keyboard_is_down(), hid_keyboard_next_down()
            break;
        case USB_INPUT_EVENT_MOUSE:
            // event->mouse.dx, .dy, .wheel are changes: add them up
            break;
        case USB_INPUT_EVENT_GAMEPAD:
            // event->gamepad, for player event->player
            break;
        case USB_INPUT_EVENT_DISCONNECTED:
            // let go of whatever event->device was holding down
            break;
        default:
            break;
    }
}

void app_main(void) {
    // On a board where the host port is switched, power it first.
    usb_input_config_t config = USB_INPUT_CONFIG_DEFAULT();
    config.callback = on_input;
    ESP_ERROR_CHECK(usb_input_start(&config));
}
```

The callback runs on the component's tasks, so it must be safe to call from more than one task and
must return quickly. A mouse can report a thousand times a second: add its movement up in the
callback rather than queueing every report.

`usb_input_stop()` takes it all down again and sends a disconnect for every device still plugged in.

## Where it comes from

The code started in the [TIC-80](https://github.com/nesbox/TIC-80) port for the badge.team Tanmatsu,
and was moved here so that other apps on the badge do not each carry their own copy.

## License

MIT
