// USB keyboards, mice and gamepads as input, on top of the ESP-IDF USB host library and its HID
// class driver. hid-host does the reading of descriptors and reports; this does the plumbing
// around it: the tasks, telling devices apart, and taking it all down again.

#include "usb_input.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hid_layout.h"
#include "usb/hid.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_mouse.h"
#include "usb/usb_host.h"

static char const TAG[] = "usb-input";

typedef struct {
    bool                     in_use;
    usb_input_kind_t         kind;
    bool                     boot;  // A mouse read from its boot report, its descriptor being no use
    uint8_t                  player;
    uint16_t                 vid;
    uint16_t                 pid;
    hid_host_device_handle_t handle;
    hid_layout_t             layout;    // Mice
    hid_gamepad_t            gamepad;   // Gamepads
    hid_keyboard_t           keyboard;  // Keyboards
} device_t;

static device_t devices[USB_INPUT_MAX_DEVICES];

static usb_input_callback_t callback     = NULL;
static void*                callback_arg = NULL;

static QueueHandle_t driver_queue = NULL;
static bool          started      = false;

// What start() got as far as putting up, so stop() can take down exactly that much: it also runs
// on the error paths, where only some of it exists.
static bool                  host_installed = false;
static bool                  hid_installed  = false;
static volatile bool         stopping       = false;
static TaskHandle_t volatile lib_task       = NULL;
static TaskHandle_t volatile hid_task       = NULL;

static void emit(const usb_input_event_t* event) {
    if (callback != NULL) {
        callback(event, callback_arg);
    }
}

static usb_input_event_t event_for(const device_t* device, usb_input_event_type_t type) {
    usb_input_event_t event = {
        .type   = type,
        .device = (uint8_t)(device - devices),
        .kind   = device->kind,
        .player = device->player,
        .vid    = device->vid,
        .pid    = device->pid,
    };
    return event;
}

// ---------------------------------------------------------------------------------------------
// Reports, on the HID driver's task
// ---------------------------------------------------------------------------------------------

// A report that is not for the keys, or one saying the keyboard has more keys down than it can
// name, is dropped: the keys that were down are still down.
static void decode_keyboard(const device_t* device, const uint8_t* data, int length) {
    usb_input_event_t event = event_for(device, USB_INPUT_EVENT_KEYBOARD);
    if (hid_keyboard_decode(&device->keyboard, data, length, &event.keyboard)) {
        emit(&event);
    }
}

static void decode_mouse_boot(const device_t* device, const uint8_t* data, int length) {
    if (length < (int)sizeof(hid_mouse_input_report_boot_t)) {
        return;
    }

    const hid_mouse_input_report_boot_t* report = (const hid_mouse_input_report_boot_t*)data;

    usb_input_event_t event = event_for(device, USB_INPUT_EVENT_MOUSE);
    event.mouse.dx          = report->x_displacement;
    event.mouse.dy          = report->y_displacement;
    event.mouse.buttons     = report->buttons.val & 0x07;
    emit(&event);
}

static void decode_mouse(const device_t* device, const uint8_t* data, int length) {
    const uint8_t* body = data;
    int            left = length;
    if (!hid_layout_strip_report_id(&device->layout, &body, &left)) {
        return;  // A report for some other part of the device, a battery level say
    }

    usb_input_event_t event = event_for(device, USB_INPUT_EVENT_MOUSE);
    event.mouse.dx          = hid_layout_read(body, left, &device->layout.x);
    event.mouse.dy          = hid_layout_read(body, left, &device->layout.y);
    event.mouse.wheel       = hid_layout_read(body, left, &device->layout.wheel);

    for (uint16_t button = 0; button < device->layout.button_count && button < 8; button++) {
        if (hid_layout_read_button(body, left, &device->layout, button)) {
            event.mouse.buttons |= (uint8_t)(1u << button);
        }
    }

    emit(&event);
}

static void decode_gamepad(const device_t* device, const uint8_t* data, int length) {
    usb_input_event_t event = event_for(device, USB_INPUT_EVENT_GAMEPAD);
    if (hid_gamepad_decode(&device->gamepad, data, length, &event.gamepad)) {
        emit(&event);
    }
}

static void release(device_t* device) {
    if (!device->in_use) {
        return;
    }
    if (device->kind != USB_INPUT_KIND_NONE) {
        usb_input_event_t event = event_for(device, USB_INPUT_EVENT_DISCONNECTED);
        emit(&event);
    }
    if (device->kind == USB_INPUT_KIND_KEYBOARD) {
        hid_keyboard_close(&device->keyboard);
    } else if (device->kind == USB_INPUT_KIND_GAMEPAD) {
        hid_gamepad_close(&device->gamepad);
    }
    device->in_use = false;
    device->kind   = USB_INPUT_KIND_NONE;
}

static void interface_callback(hid_host_device_handle_t handle, const hid_host_interface_event_t event, void* arg) {
    device_t* device = (device_t*)arg;

    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
            uint8_t data[64];
            size_t  length = 0;
            if (hid_host_device_get_raw_input_report_data(handle, data, sizeof(data), &length) != ESP_OK) {
                return;
            }

            switch (device->kind) {
                case USB_INPUT_KIND_KEYBOARD:
                    decode_keyboard(device, data, (int)length);
                    break;
                case USB_INPUT_KIND_MOUSE:
                    if (device->boot) {
                        decode_mouse_boot(device, data, (int)length);
                    } else {
                        decode_mouse(device, data, (int)length);
                    }
                    break;
                case USB_INPUT_KIND_GAMEPAD:
                    decode_gamepad(device, data, (int)length);
                    break;
                default:
                    break;
            }
            break;
        }

        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Device disconnected");
            hid_host_device_close(handle);
            release(device);
            break;

        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
            ESP_LOGW(TAG, "Transfer error");
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------
// Devices coming in, on the component's own task
// ---------------------------------------------------------------------------------------------

static device_t* take_slot(void) {
    for (size_t i = 0; i < USB_INPUT_MAX_DEVICES; i++) {
        if (!devices[i].in_use) {
            memset(&devices[i], 0, sizeof(devices[i]));
            devices[i].in_use = true;
            return &devices[i];
        }
    }
    return NULL;
}

// A pad keeps its player for as long as it stays plugged in, so unplugging player one leaves
// player two where it was rather than promoting it.
static uint8_t take_player(const device_t* self) {
    for (uint8_t player = 0; player < USB_INPUT_MAX_PLAYERS; player++) {
        bool taken = false;
        for (size_t i = 0; i < USB_INPUT_MAX_DEVICES; i++) {
            if (&devices[i] != self && devices[i].in_use && devices[i].kind == USB_INPUT_KIND_GAMEPAD &&
                devices[i].player == player) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return player;
        }
    }
    return 0;  // Cannot happen while there are no more slots than players
}

// Some gamepads enumerate, hand out a report descriptor and then say nothing at all until they
// are asked to start reporting. hid-host knows which ones and what to send; sending it is up to
// whoever holds the handle.
static void nudge_gamepad(hid_host_device_handle_t handle, const hid_gamepad_t* gamepad) {
    const hid_gamepad_quirk_t* quirk = gamepad->quirk;
    if (quirk == NULL || quirk->enable_report == NULL) {
        return;
    }

    uint8_t report[8] = {0};
    size_t  length    = quirk->enable_report_length;
    if (length > sizeof(report)) {
        length = sizeof(report);
    }
    memcpy(report, quirk->enable_report, length);

    ESP_LOGI(TAG, "%s needs a nudge before it reports, sending it", quirk->name);
    hid_class_request_set_report(handle, HID_REPORT_TYPE_FEATURE, quirk->enable_report_id, report, length);
}

static void device_connected(hid_host_device_handle_t handle) {
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(handle, &params) != ESP_OK) {
        return;
    }

    device_t* device = take_slot();
    if (device == NULL) {
        ESP_LOGW(TAG, "No room for another device");
        return;
    }
    device->handle = handle;

    const hid_host_device_config_t config = {.callback = interface_callback, .callback_arg = device};
    if (hid_host_device_open(handle, &config) != ESP_OK) {
        device->in_use = false;
        return;
    }

    hid_host_dev_info_t info;
    if (hid_host_get_device_info(handle, &info) == ESP_OK) {
        device->vid = info.VID;
        device->pid = info.PID;
    }

    bool boot = params.sub_class == HID_SUBCLASS_BOOT_INTERFACE;

    if (boot && params.proto == HID_PROTOCOL_KEYBOARD) {
        // The boot report is a fixed eight bytes and every keyboard sends it, so the boot
        // interface is read in that shape and nothing is parsed.
        hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT);
        hid_keyboard_open_boot(&device->keyboard);
        device->kind = USB_INPUT_KIND_KEYBOARD;
    } else {
        if (boot) {
            // Ask a boot mouse for its full reports: that is where the wheel and the fourth and
            // fifth buttons live.
            hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_REPORT);
        }

        size_t         length     = 0;
        const uint8_t* descriptor = hid_host_get_report_descriptor(handle, &length);

        if (descriptor != NULL && hid_gamepad_open(&device->gamepad, descriptor, length, device->vid, device->pid)) {
            device->kind   = USB_INPUT_KIND_GAMEPAD;
            device->player = take_player(device);
            nudge_gamepad(handle, &device->gamepad);
        } else if (descriptor != NULL && hid_layout_parse(descriptor, length, &device->layout) &&
                   ((device->layout.x.present && device->layout.x.relative) ||
                    (device->layout.y.present && device->layout.y.relative))) {
            // What separates a mouse from the rest is that it reports a change rather than a
            // position. Without that test the second interface of a keyboard, whose keys parse
            // as a pile of buttons, would be taken for a mouse and click on every keystroke.
            device->kind = USB_INPUT_KIND_MOUSE;
        } else if (descriptor != NULL && hid_keyboard_open(&device->keyboard, descriptor, length)) {
            // Keys on an interface that is not the boot keyboard: often the second interface of
            // the same keyboard, where it reports a bitmap so that more than six keys can be down
            // at once. It is a device of its own, with its own slot and its own key state.
            device->kind = USB_INPUT_KIND_KEYBOARD;
        } else if (params.proto == HID_PROTOCOL_MOUSE) {
            device->kind = USB_INPUT_KIND_MOUSE;
            device->boot = true;
        } else {
            ESP_LOGI(TAG, "Nothing usable in the report descriptor, ignoring the device");
            hid_host_device_close(handle);
            device->in_use = false;
            return;
        }
    }

    if (device->kind != USB_INPUT_KIND_GAMEPAD) {
        // Report on change rather than on a timer. A report says the whole state, so a repeat
        // changes nothing, but there is no reason to wake for one either.
        hid_class_request_set_idle(handle, 0, 0);
    }

    static const char* const kind_name[] = {"nothing", "keyboard", "mouse", "gamepad"};
    ESP_LOGI(TAG, "%04x:%04x connected, using it as a %s", device->vid, device->pid, kind_name[device->kind]);

    usb_input_event_t event = event_for(device, USB_INPUT_EVENT_CONNECTED);
    emit(&event);

    hid_host_device_start(handle);
}

// The driver calls this from its own task, where opening a device would deadlock, so it only
// hands the event on.
static void driver_callback(hid_host_device_handle_t handle, const hid_host_driver_event_t event, void* arg) {
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED || driver_queue == NULL) {
        return;
    }
    xQueueSend(driver_queue, &handle, 0);
}

static void driver_task(void* arg) {
    hid_host_device_handle_t handle;
    while (xQueueReceive(driver_queue, &handle, portMAX_DELAY) == pdTRUE) {
        if (handle == NULL) {
            break;  // The stop token usb_input_stop() sends
        }
        device_connected(handle);
    }
    hid_task = NULL;
    vTaskDelete(NULL);
}

static void usb_lib_task(void* arg) {
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };

    if (usb_host_install(&config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install the USB host library");
        lib_task = NULL;
        xTaskNotifyGive((TaskHandle_t)arg);
        vTaskDelete(NULL);
        return;
    }

    started        = true;
    host_installed = true;
    xTaskNotifyGive((TaskHandle_t)arg);

    // usb_host_lib_unblock() is what returns this from its wait on the way out.
    while (!stopping) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }

    lib_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------------------------
// Start and stop
// ---------------------------------------------------------------------------------------------

esp_err_t usb_input_start(const usb_input_config_t* config) {
    if (config == NULL || config->callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (started || hid_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(devices, 0, sizeof(devices));
    callback     = config->callback;
    callback_arg = config->callback_arg;
    stopping     = false;

    driver_queue = xQueueCreate(USB_INPUT_MAX_DEVICES, sizeof(hid_host_device_handle_t));
    if (driver_queue == NULL) {
        usb_input_stop();
        return ESP_ERR_NO_MEM;
    }

    // The library task only moves events along; it needs to run, not to run first.
    unsigned     priority = config->task_priority > 2 ? config->task_priority : 3;
    TaskHandle_t task     = NULL;
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb-input-lib", 4096, xTaskGetCurrentTaskHandle(), 2, &task,
                                config->core_id) != pdPASS) {
        usb_input_stop();
        return ESP_ERR_NO_MEM;
    }
    lib_task = task;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

    if (!started) {
        usb_input_stop();
        return ESP_FAIL;
    }

    const hid_host_driver_config_t driver_config = {
        .create_background_task = true,
        .task_priority          = priority,
        .stack_size             = 4096,
        .core_id                = config->core_id,
        .callback               = driver_callback,
        .callback_arg           = NULL,
    };

    esp_err_t res = hid_host_install(&driver_config);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install the HID driver: %s", esp_err_to_name(res));
        usb_input_stop();
        return res;
    }
    hid_installed = true;

    task = NULL;
    if (xTaskCreatePinnedToCore(driver_task, "usb-input-hid", 4096, NULL, priority - 1, &task, config->core_id) !=
        pdPASS) {
        usb_input_stop();
        return ESP_ERR_NO_MEM;
    }
    hid_task = task;

    ESP_LOGI(TAG, "Started");
    return ESP_OK;
}

// Neither task is deleted from the outside: one is inside the USB host library and the other may
// be halfway through opening a device. They are asked to leave and clear their own handle, and
// this waits for that.
static void join(TaskHandle_t volatile* task) {
    for (int i = 0; i < 100 && *task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (*task != NULL) {
        ESP_LOGW(TAG, "A USB task did not stop, leaving it be");
        *task = NULL;
    }
}

void usb_input_stop(void) {
    stopping = true;

    if (hid_task != NULL && driver_queue != NULL) {
        hid_host_device_handle_t stop = NULL;
        xQueueSend(driver_queue, &stop, 0);
        join(&hid_task);
    }

    // In this order: the driver holds a client of the host library, and the library will not
    // uninstall while a client is registered.
    if (hid_installed) {
        hid_host_uninstall();
        hid_installed = false;
    }

    // With the driver gone no report or disconnect can arrive any more, so whatever is still
    // plugged in is let go of here, from the caller, without racing the driver's task.
    for (size_t i = 0; i < USB_INPUT_MAX_DEVICES; i++) {
        release(&devices[i]);
    }

    if (lib_task != NULL) {
        usb_host_lib_unblock();
        join(&lib_task);
    }

    if (host_installed) {
        usb_host_uninstall();
        host_installed = false;
    }

    started = false;

    if (driver_queue != NULL) {
        vQueueDelete(driver_queue);
        driver_queue = NULL;
    }

    memset(devices, 0, sizeof(devices));
    callback     = NULL;
    callback_arg = NULL;
    stopping     = false;
}
