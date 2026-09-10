if(CONFIG_ZMK_BATTERY_REPORT)
    zephyr_include_directories(include)
    target_sources(app PRIVATE src/battery_report.c)

    if(CONFIG_ZMK_BATTERY_REPORT_STUDIO_RPC)
        target_sources(app PRIVATE src/studio/battery_report_subsystem.c)
    endif()
endif()
