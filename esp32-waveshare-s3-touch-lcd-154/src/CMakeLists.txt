set(COMMON_SRC "webrtc.cpp" "main.cpp" "http.cpp")

if(IDF_TARGET STREQUAL linux)
	idf_component_register(
		SRCS ${COMMON_SRC}
		REQUIRES peer esp-libopus esp_http_client json)
else()
	idf_component_register(
		SRCS ${COMMON_SRC} "wifi.cpp" "media.cpp" "rtvi.cpp" "rtvi_callbacks.cpp" "screen.cpp" "button.cpp" "board.cpp"
		REQUIRES driver esp_wifi nvs_flash peer esp_psram esp-libopus esp_http_client json esp_lcd mbedtls esp_timer)
endif()

idf_component_get_property(lib peer COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wno-error=restrict)
target_compile_options(${lib} PRIVATE -Wno-error=stringop-truncation)
# The shared peer component turns libpeer's keepalive check off (0). Turn it
# back on: without it the board never notices the bot hanging up and stays in
# a dead call. 10 s with no ICE check from the bot ends the call.
target_compile_options(${lib} PRIVATE -UCONFIG_KEEPALIVE_TIMEOUT -DCONFIG_KEEPALIVE_TIMEOUT=10000)

idf_component_get_property(lib srtp COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wno-error=incompatible-pointer-types)

idf_component_get_property(lib esp-libopus COMPONENT_LIB)
target_compile_options(${lib} PRIVATE -Wno-error=maybe-uninitialized)
target_compile_options(${lib} PRIVATE -Wno-error=stringop-overread)
