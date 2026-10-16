idf_component_register(
    SRCS "main.c" "plant_ui.c" "plant_wifi.c" "board_display.c" "plant_camera.c"
         "plant_input.c" "camera_settings.c" "plant_cloud.c" "photo_store.c"
         "voice_tts.c" "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES esp_iris esp-mosaico-bsp esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris freertos nvs_flash
        esp_wifi esp_netif esp_event mosaico_module_camera mosaico_module_mgr
    PRIV_REQUIRES esp_timer esp-gsp esp_display_present esp_lcd esp_driver_ppa
        esp_http_client esp_mm cjson mbedtls esp_partition esp_codec_dev
        esp-sr esp_new_jpeg)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "../ui/main.json")

idf_build_get_property(build_components BUILD_COMPONENTS)
if(esp-sr IN_LIST build_components)
    set(esp_sr_component esp-sr)
else()
    set(esp_sr_component espressif__esp-sr)
endif()
idf_component_get_property(esp_sr_path ${esp_sr_component} COMPONENT_DIR)
set(voice_data_image
    "${esp_sr_path}/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaole.dat")
add_custom_target(voice_data ALL DEPENDS "${voice_data_image}")
set_property(GLOBAL APPEND PROPERTY
    MOSAICO_SYSTEM_UPDATE_DATA_LABELS voice_data)
set_property(GLOBAL PROPERTY
    MOSAICO_SYSTEM_UPDATE_DATA_voice_data_IMAGE "${voice_data_image}")
set_property(GLOBAL PROPERTY
    MOSAICO_SYSTEM_UPDATE_DATA_voice_data_TARGET voice_data)
add_dependencies(flash voice_data)
partition_table_get_partition_info(voice_data_size
    "--partition-name voice_data" "size")
partition_table_get_partition_info(voice_data_offset
    "--partition-name voice_data" "offset")
if("${voice_data_size}" AND "${voice_data_offset}")
    esptool_py_flash_to_partition(flash "voice_data" "${voice_data_image}")
else()
    message(FATAL_ERROR "Partition table must contain voice_data")
endif()
