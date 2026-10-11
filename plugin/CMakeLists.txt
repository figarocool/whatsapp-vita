cmake_minimum_required(VERSION 3.10)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain")
endif()
project(wa_notify C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -std=gnu11")

add_executable(wa_notify src/plugin.c src/mini.c)
set_source_files_properties(src/mini.c PROPERTIES COMPILE_FLAGS "-fno-builtin -fno-tree-loop-distribute-patterns")
target_include_directories(wa_notify PRIVATE ../app/src)
target_link_libraries(wa_notify
  taihen_stub SceSysmodule_stub SceKernelModulemgr_stub
  SceIofilemgr_stub SceKernelThreadMgr_stub SceProcessmgr_stub SceLibKernel_stub gcc)
set_target_properties(wa_notify PROPERTIES LINK_FLAGS "-nostdlib")
vita_create_self(wa_notify.suprx wa_notify CONFIG exports.yml UNSAFE)

# plugin di prova a livelli (0..3): vedi src/probe.c
foreach(N 0 1 2 3)
  add_executable(wa_probe${N} src/probe.c src/mini.c)
  set_source_files_properties(src/mini.c PROPERTIES COMPILE_FLAGS "-fno-builtin -fno-tree-loop-distribute-patterns")
  target_compile_definitions(wa_probe${N} PRIVATE STAGE=${N})
  target_link_libraries(wa_probe${N} taihen_stub SceNet_stub SceSysmodule_stub SceIofilemgr_stub SceKernelThreadMgr_stub SceProcessmgr_stub SceLibKernel_stub gcc)
  set_target_properties(wa_probe${N} PROPERTIES LINK_FLAGS "-nostdlib")
  vita_create_self(wa_probe${N}.suprx wa_probe${N} CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/probe/exports_${N}.yml UNSAFE)
endforeach()

add_executable(wa_min src/probe_min.c)
target_link_libraries(wa_min SceIofilemgr_stub SceLibKernel_stub gcc)
set_target_properties(wa_min PROPERTIES LINK_FLAGS "-nostdlib")
vita_create_self(wa_min.suprx wa_min CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/probe/exports_min.yml UNSAFE)
