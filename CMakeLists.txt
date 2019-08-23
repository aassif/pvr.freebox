cmake_minimum_required(VERSION 3.5)
project(pvr.freebox)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR})

find_package(Kodi REQUIRED)
find_package(kodiplatform REQUIRED)
find_package(p8-platform REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(RapidJSON REQUIRED)

include_directories(${kodiplatform_INCLUDE_DIRS}
                    ${p8-platform_INCLUDE_DIRS}
                    ${KODI_INCLUDE_DIR}/.. # Hack way with "/..", need bigger Kodi cmake rework to match right include ways
                    ${OPENSSL_INCLUDE_DIRS}
                    ${ZLIB_INCLUDE_DIRS}
                    ${RAPIDJSON_INCLUDE_DIRS})

set(DEPLIBS ${p8-platform_LIBRARIES} ${OPENSSL_LIBRARIES} ${ZLIB_LIBRARIES})

set(FREEBOX_SOURCES src/client.cpp
                    src/Freebox.cpp)

set(FREEBOX_HEADERS src/client.h
                    src/Freebox.h)

build_addon(pvr.freebox FREEBOX DEPLIBS)

set_property(TARGET pvr.freebox PROPERTY CXX_STANDARD 17)

include(CPack)
