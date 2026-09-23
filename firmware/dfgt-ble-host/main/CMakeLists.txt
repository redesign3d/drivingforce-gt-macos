idf_component_register(
    SRCS
        "app_main.c"
        "dfgt_decode.c"
        "dfgt_usb.c"
        "pad_map.c"
        "ble_gamepad.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_hid          # BLE HID device (esp_hidd) + report map parsing
        usb              # USB host library (usb_host.h, usb_helpers.h)
        bt               # Bluedroid + GAP/GATTS
        esp_timer
        nvs_flash
        driver           # UART0 console input
)
