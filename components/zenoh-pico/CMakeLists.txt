# ESP-IDF wrapper for zenoh-pico.
#
# The upstream source is a git submodule under upstream/.  We keep our own
# wrapper so the component can add the IDF dependencies (lwip, esp_timer) and
# so it compiles to nothing unless the Zenoh backend is selected in Kconfig.

if(CONFIG_ROBOT_MIDDLEWARE_ZENOH)
    file(GLOB_RECURSE ZENOH_SOURCES
        "upstream/src/api/*.c"
        "upstream/src/collections/*.c"
        "upstream/src/link/*.c"
        "upstream/src/net/*.c"
        "upstream/src/protocol/*.c"
        "upstream/src/session/*.c"
        "upstream/src/transport/*.c"
        "upstream/src/utils/*.c"
        "upstream/src/system/espidf/*.c"
        "upstream/src/system/common/*.c"
        "upstream/src/system/socket/*.c"
        "upstream/src/runtime/*.c"
    )

    idf_component_register(
        SRCS ${ZENOH_SOURCES}
        INCLUDE_DIRS
            "upstream/include"
            "upstream/include/zenoh-pico"
        REQUIRES
            lwip
            esp_timer
        PRIV_REQUIRES
            esp_driver_uart
    )

    target_compile_definitions(${COMPONENT_LIB} PUBLIC ZENOH_ESPIDF)
    # Upstream zenoh-pico 1.9.0 has one maybe-uninitialized warning; do not
    # turn it into a build failure for a third-party source file.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=maybe-uninitialized)

    # esp_driver_uart is pulled in late by the IDF requirement resolver; link
    # it explicitly so platform/espidf.h can always find driver/uart.h.
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_driver_uart)
else()
    idf_component_register()
endif()
