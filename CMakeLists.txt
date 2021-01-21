cmake_minimum_required(VERSION 3.5)
project(pvr.freebox)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR})

find_package(Kodi REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json REQUIRED)

include_directories(${KODI_INCLUDE_DIR}/.. # Hack way with "/..", need bigger Kodi cmake rework to match right include ways
                    ${OPENSSL_INCLUDE_DIRS}
                    ${ZLIB_INCLUDE_DIRS}
                    ${NLOHMANNJSON_INCLUDE_DIRS})

set(DEPLIBS ${OPENSSL_LIBRARIES} ${ZLIB_LIBRARIES})

if(WIN32)
  list(APPEND DEPLIBS ws2_32)
endif()

set(FREEBOX_SOURCES src/Freebox.cpp)

set(FREEBOX_HEADERS src/Freebox.h)

addon_version(pvr.freebox FREEBOX)
add_definitions(-DFREEBOX_VERSION=${FREEBOX_VERSION})

build_addon(pvr.freebox FREEBOX DEPLIBS)

include(CPack)
