cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
      set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
          CACHE PATH "VitaSDK toolchain")
    else()
      message(FATAL_ERROR "VITASDK is required")
    endif()
  endif()
  project(vita_sw_decoder VERSION 2.0.0 LANGUAGES C)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(VITA_SW_DECODER_FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/build/deps/ffmpeg-vita-sw"
    CACHE PATH "Pinned software FFmpeg installation")
option(VITA_SW_DECODER_BUILD_EXAMPLE "Compile the local-file example" OFF)

if(NOT EXISTS "${VITA_SW_DECODER_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
  message(FATAL_ERROR
    "Software FFmpeg was not found at ${VITA_SW_DECODER_FFMPEG_ROOT}. "
    "Run tools/build-ffmpeg.sh or set VITA_SW_DECODER_FFMPEG_ROOT.")
endif()

add_library(vita_sw_decoder STATIC
  src/vita_sw_decoder.c
  src/file_stream.c
  src/module_log.c
  src/decoder_runtime.c
  src/internal/software_video.c
  src/internal/elementary_audio.c
  src/internal/vita_aac_decoder.c)
add_library(VitaSwDecoder::VitaSwDecoder ALIAS vita_sw_decoder)
set_target_properties(vita_sw_decoder PROPERTIES
  EXPORT_NAME VitaSwDecoder
  VERSION 2.0.0
  SOVERSION 2)

target_include_directories(vita_sw_decoder
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${VITA_SW_DECODER_FFMPEG_ROOT}/include)
target_link_directories(vita_sw_decoder PUBLIC
  $<BUILD_INTERFACE:${VITA_SW_DECODER_FFMPEG_ROOT}/lib>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_LIBDIR}/vita-sw-decoder>)
target_link_libraries(vita_sw_decoder PUBLIC
  avformat avcodec avutil vita2d
  SceAudiodec_stub SceGxm_stub SceAppMgr_stub SceCommonDialog_stub
  SceAudio_stub SceDisplay_stub pthread m z)
target_compile_features(vita_sw_decoder PUBLIC c_std_11)
target_compile_definitions(vita_sw_decoder PRIVATE
  VITA_SW_DECODER_HAS_H264_VITA=0
  VITA_SW_DECODER_ALLOW_SOFTWARE_FALLBACK=1)
target_compile_options(vita_sw_decoder PRIVATE
  -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
  -O3 -ffunction-sections -fdata-sections)

if(VITA_SW_DECODER_BUILD_EXAMPLE)
  add_library(vita_sw_decoder_local_example OBJECT examples/local_file.c)
  target_link_libraries(vita_sw_decoder_local_example PRIVATE VitaSwDecoder::VitaSwDecoder)
endif()

install(TARGETS vita_sw_decoder EXPORT VitaSwDecoderTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/vita_sw_decoder.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES
  "${VITA_SW_DECODER_FFMPEG_ROOT}/lib/libavformat.a"
  "${VITA_SW_DECODER_FFMPEG_ROOT}/lib/libavcodec.a"
  "${VITA_SW_DECODER_FFMPEG_ROOT}/lib/libavutil.a"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/vita-sw-decoder)
install(EXPORT VitaSwDecoderTargets NAMESPACE VitaSwDecoder::
  FILE VitaSwDecoderTargets.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VitaSwDecoder)
configure_package_config_file(cmake/VitaSwDecoderConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/VitaSwDecoderConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VitaSwDecoder)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/VitaSwDecoderConfigVersion.cmake
  VERSION 2.0.0 COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/VitaSwDecoderConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/VitaSwDecoderConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VitaSwDecoder)
install(FILES LICENSE THIRD_PARTY_NOTICES.md DEPENDENCIES.lock
  DESTINATION share/licenses/vita-sw-decoder)
install(DIRECTORY "${VITA_SW_DECODER_FFMPEG_ROOT}/share/licenses/"
  DESTINATION share/licenses/vita-sw-decoder/third-party)
install(DIRECTORY "${VITA_SW_DECODER_FFMPEG_ROOT}/share/sources/"
  DESTINATION share/sources/vita-sw-decoder/third-party)
