cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(surf_vita C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Surf for Vita")
set(VITA_TITLEID  "SURF00001")
set(VITA_VERSION  "00.10")

option(SV_STUB_SESSION "Build with the fake session (no backend needed)" OFF)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2 -D_GNU_SOURCE")

set(APP_SOURCES src/main.c src/decoder.c)

if(SV_STUB_SESSION)
  set(SESSION_SOURCES src/session_stub.c)
else()
  file(GLOB SESSION_SOURCES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/src/session.c"
       "${CMAKE_CURRENT_SOURCE_DIR}/src/session_*.c")
  list(FILTER SESSION_SOURCES EXCLUDE REGEX ".*/session_stub\\.c$")
  if(NOT SESSION_SOURCES)
    message(FATAL_ERROR "No src/session.c or src/session_*.c: configure with -DSV_STUB_SESSION=ON")
  endif()
  file(GLOB SURF_CORE_SOURCES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/../upstream/client/core/src/*.c")
  list(APPEND SESSION_SOURCES ${SURF_CORE_SOURCES})
endif()

add_executable(${PROJECT_NAME} ${APP_SOURCES} ${SESSION_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE src)
if(NOT SV_STUB_SESSION)
  target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../upstream/client/core/include")
endif()

target_link_libraries(${PROJECT_NAME}
  vita2d
  png
  jpeg
  z
  m
  mbedtls
  mbedx509
  mbedcrypto
  SceVideodec_stub
  ScePgf_stub
  ScePvf_stub
  SceDisplay_stub
  SceGxm_stub
  SceCtrl_stub
  SceTouch_stub
  SceNet_stub
  SceNetCtl_stub
  SceSysmodule_stub
  SceCommonDialog_stub
  SceAppUtil_stub
  SceAppMgr_stub
  ScePower_stub
  SceIofilemgr_stub
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceSysmem_stub
  pthread
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
