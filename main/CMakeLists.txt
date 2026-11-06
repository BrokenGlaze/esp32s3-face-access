# 自动收集所有 C++ 源文件
file(GLOB_RECURSE ALL_C_SRCS   "./*.c")
file(GLOB_RECURSE ALL_CPP_SRCS "main.c" "./*.cpp")

# 自动收集 audio 目录下的所有 PCM 提示音；新增音频文件无需再改本文件。
# 注意：新增/删除 PCM 后需执行一次 `idf.py reconfigure` 让 CMake 重新扫描目录。
file(GLOB AUDIO_PCM_FILES "speech_recognition/audio/*.pcm")

idf_component_register(SRCS ${ALL_C_SRCS} ${ALL_CPP_SRCS}
                       INCLUDE_DIRS "." "lvgl"
                                    "speech_recognition/i2s"
                                    "speech_recognition/es8311"
                                    "speech_recognition/audio"
                                    "i2c" "uart"
                                    "wifi"
                                    "cloud"
                                    "who_human_face_detection" "font"
                       PRIV_REQUIRES nvs_flash esp_driver_gpio esp-dl
                                     esp_lvgl_port lvgl esp_lcd_touch
                                     fatfs wear_levelling
                                     esp_wifi esp_netif esp_event lwip
                                     esp_http_server mqtt
                                     esp_http_client esp-tls mbedtls esp32-camera
                       EMBED_FILES ${AUDIO_PCM_FILES})
