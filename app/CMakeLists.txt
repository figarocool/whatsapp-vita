cmake_minimum_required(VERSION 3.10)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain")
endif()
project(whatsapp_vita C)
# Versione dell'app (tag della release, es. v1.0.0): finisce in assets/version.txt e serve a capire se c'e' un aggiornamento. "dev" = build locale, mai sostituita.
set(WA_VERSION "dev" CACHE STRING "Versione incorporata nel VPK")
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "WhatsApp")
set(VITA_TITLEID  "WHAP00001")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -std=gnu11")

set(ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../engine)
set(ENGINE_SRCS ${ENGINE_DIR}/src/engine.c ${ENGINE_DIR}/src/curve25519.c ${ENGINE_DIR}/src/image.c ${ENGINE_DIR}/src/audio.c ${ENGINE_DIR}/quickjs/quickjs.c ${ENGINE_DIR}/quickjs/cutils.c ${ENGINE_DIR}/quickjs/dtoa.c ${ENGINE_DIR}/quickjs/libregexp.c ${ENGINE_DIR}/quickjs/libunicode.c)
set_source_files_properties(${ENGINE_SRCS} PROPERTIES COMPILE_FLAGS "-w -DPATH_MAX=256 -DCONFIG_VERSION=\\\"engine\\\"")
include_directories(${ENGINE_DIR}/src ${ENGINE_DIR}/quickjs)
set(SRCS ${ENGINE_SRCS} src/main.c src/json.c src/net.c src/updater.c src/player.c src/i18n.c src/i18n_data.c src/sharedfb_stub.c)
add_executable(${PROJECT_NAME} ${SRCS})
add_executable(whatsapp_safe ${SRCS})
target_compile_definitions(whatsapp_safe PRIVATE SAFE_BUILD=1)
target_link_libraries(${PROJECT_NAME}
  vita2d curl mbedtls mbedx509 mbedcrypto freetype png jpeg webpdemux webp opus ogg zstd bz2 z m pthread SceAppMgr_stub
  SceDisplay_stub SceGxm_stub SceCtrl_stub SceTouch_stub SceSysmodule_stub
  SceCommonDialog_stub SceIme_stub SceCamera_stub SceAppUtil_stub SceNet_stub SceNetCtl_stub
  SceNotificationUtil_stub ScePromoterUtil_stub SceAvPlayer_stub SceAudio_stub
  SceHttp_stub SceSsl_stub SceKernelThreadMgr_stub SceRtc_stub ScePower_stub)
target_link_libraries(whatsapp_safe
  vita2d curl mbedtls mbedx509 mbedcrypto freetype png jpeg webpdemux webp opus ogg zstd bz2 z m pthread SceAppMgr_stub
  SceDisplay_stub SceGxm_stub SceCtrl_stub SceTouch_stub SceSysmodule_stub
  SceCommonDialog_stub SceIme_stub SceCamera_stub SceAppUtil_stub SceNet_stub SceNetCtl_stub SceAvPlayer_stub SceAudio_stub
  SceHttp_stub SceSsl_stub SceKernelThreadMgr_stub SceRtc_stub ScePower_stub)


file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/version.txt "${WA_VERSION}\n")
set(COMMON_FILES
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE assets/Roboto-Regular.ttf assets/Roboto-Regular.ttf
  FILE assets/emoji.png assets/emoji.png
  FILE assets/author.png assets/author.png
  FILE assets/emoji.bin assets/emoji.bin
  FILE assets/cacert.pem assets/cacert.pem
  FILE ${ENGINE_DIR}/js/bridge.min.qbc assets/engine.qbc
  FILE assets/font-ja.ttf assets/font-ja.ttf
  FILE assets/font-ko.ttf assets/font-ko.ttf
  FILE assets/font-zhs.ttf assets/font-zhs.ttf
  FILE assets/font-zht.ttf assets/font-zht.ttf
  FILE ${CMAKE_CURRENT_BINARY_DIR}/version.txt assets/version.txt)

# Installatore (WHAPUPD01): app separata che chiude WhatsApp, installa il pacchetto scaricato e la riapre.
add_executable(wa_updater helper/updater_helper.c)
target_compile_options(wa_updater PRIVATE -Wall -O2)
target_link_libraries(wa_updater mbedcrypto z m SceDisplay_stub SceSysmem_stub SceAppMgr_stub ScePromoterUtil_stub SceSysmodule_stub SceIofilemgr_stub SceLibKernel_stub SceKernelModulemgr_stub SceKernelThreadMgr_stub)
vita_create_self(wa-updater-eboot.bin wa_updater UNSAFE)
set(VITA_PACK_VPK_FLAGS "")   # vita_create_vpk accumula i file di una chiamata nella successiva: si azzera ogni volta
file(GLOB UPDATE_SCREENS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/assets/update-screen-*.z)
set(UPDATE_SCREEN_FILES "")
foreach(f ${UPDATE_SCREENS})
  list(APPEND UPDATE_SCREEN_FILES FILE ${f} ${f})
endforeach()
vita_create_vpk(wa-updater.vpk WHAPUPD01 wa-updater-eboot.bin VERSION "01.00" NAME "WhatsApp Updater"
  FILE assets/vitashell-head.bin assets/vitashell-head.bin
  ${UPDATE_SCREEN_FILES})

# versione completa: permessi estesi, puo' installare il plugin notifiche e aggiornarsi da sola
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
set(VITA_PACK_VPK_FLAGS "")
vita_create_vpk(${PROJECT_NAME}.vpk "WHAP00001" ${PROJECT_NAME}.self
  VERSION "01.00" NAME "WhatsApp" ${COMMON_FILES}
  FILE assets/vitashell-head.bin assets/vitashell-head.bin
  FILE ${CMAKE_CURRENT_BINARY_DIR}/wa-updater.vpk assets/wa-updater.vpk
  FILE ../plugin/build/wa_notify.suprx plugin/wa_notify.suprx)
add_dependencies(${PROJECT_NAME}.vpk-vpk wa-updater.vpk-vpk)

# versione sicura: si installa senza "unsafe homebrew"; controlla gli aggiornamenti ma non puo' installarli
vita_create_self(whatsapp_safe.self whatsapp_safe)
set(VITA_PACK_VPK_FLAGS "")
vita_create_vpk(whatsapp_safe.vpk "WHAP00002" whatsapp_safe.self
  VERSION "01.00" NAME "WhatsApp sicura" ${COMMON_FILES})
