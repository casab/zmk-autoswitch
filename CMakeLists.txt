# AutoSwitch KVM for ZMK.
if (CONFIG_ZMK_AUTOSWITCH)
  zephyr_library()
  zephyr_library_sources(src/as_hidpp.c src/as_ble.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_AUTOSWITCH_USB src/as_usb.c)
  zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()
