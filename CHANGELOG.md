# Changelog

Notable changes per release, newest first. Versions follow
[semantic versioning](https://semver.org), and each release is tagged `vX.Y.Z` and published to the
ESP Component Registry as `badgeteam/usb-input`.

## 0.1.0

### Added

- `usb_input_start()` and `usb_input_stop()`: the USB host library and the HID class driver on two
  tasks, with a teardown that waits for both and sends a disconnect for every device still in use.
- Keyboards from the boot interface in boot protocol, and from any other interface whose report
  descriptor names keys, decoded with hid-host 0.4's keyboard decoder.
- Mice from their report descriptor, telling them from a keyboard's extra interface by relative
  axes, with the boot report as the fallback for a mouse whose descriptor says nothing usable.
- Gamepads through hid-host's gamepad decoder, the enable report sent to the pads that need one,
  and player numbers that stay put while a pad stays plugged in.
