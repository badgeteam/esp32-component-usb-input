idf_component_register(
    SRCS "src/usb_input.c"
    INCLUDE_DIRS "include"
    REQUIRES usb_host_hid hid-host
    PRIV_REQUIRES usb
)
