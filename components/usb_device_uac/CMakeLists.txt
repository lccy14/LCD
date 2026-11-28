set(priv_requires esp_timer esp_tinyusb)

if(${IDF_VERSION_MAJOR} LESS 6)
    list(APPEND priv_requires usb)
endif()

idf_component_register(SRCS "usb_device_uac.c"
                    INCLUDE_DIRS "include" "tusb_uac"
                    PRIV_REQUIRES ${priv_requires})
