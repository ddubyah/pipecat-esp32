cmake_minimum_required(VERSION 3.19)

if(NOT IDF_TARGET STREQUAL linux)
  if(NOT DEFINED ENV{WIFI_SSID} OR NOT DEFINED ENV{WIFI_PASSWORD})
    message(FATAL_ERROR "Env variables WIFI_SSID and WIFI_PASSWORD must be set")
  endif()

  add_compile_definitions(WIFI_SSID="$ENV{WIFI_SSID}")
  add_compile_definitions(WIFI_PASSWORD="$ENV{WIFI_PASSWORD}")
endif()

if(NOT DEFINED ENV{PIPECAT_SMALLWEBRTC_URL})
  message(FATAL_ERROR "Env variable PIPECAT_SMALLWEBRTC_URL must be set")
endif()

if(DEFINED ENV{LOG_DATACHANNEL_MESSAGES})
  add_compile_definitions(LOG_DATACHANNEL_MESSAGES="1")
endif()

add_compile_definitions(PIPECAT_SMALLWEBRTC_URL="$ENV{PIPECAT_SMALLWEBRTC_URL}")

# Optional: sent as `Authorization: Bearer <BOT_TOKEN>` with the offer (the cloud bot checks it)
if(DEFINED ENV{BOT_TOKEN} AND NOT "$ENV{BOT_TOKEN}" STREQUAL "")
  add_compile_definitions(PIPECAT_BOT_TOKEN="$ENV{BOT_TOKEN}")
endif()

set(COMPONENTS src)
set(EXTRA_COMPONENT_DIRS "src" "../esp32-s3-box-3/components/srtp" "../esp32-s3-box-3/components/peer" "../esp32-s3-box-3/components/esp-libopus")

if(IDF_TARGET STREQUAL linux)
  add_compile_definitions(LINUX_BUILD=1)
  list(APPEND EXTRA_COMPONENT_DIRS
    $ENV{IDF_PATH}/examples/protocols/linux_stubs/esp_stubs
    "../esp32-s3-box-3/components/esp-protocols/common_components/linux_compat/esp_timer"
    "../esp32-s3-box-3/components/esp-protocols/common_components/linux_compat/freertos"
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(src)
