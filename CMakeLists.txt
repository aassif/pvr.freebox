project(pvr.freebox)

cmake_minimum_required(VERSION 2.6)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR})

enable_language(CXX)

find_package(Kodi REQUIRED)
find_package(kodiplatform REQUIRED)
find_package(p8-platform REQUIRED)
find_package(RapidJSON REQUIRED)

include_directories(${kodiplatform_INCLUDE_DIRS}
                    ${p8-platform_INCLUDE_DIRS}
                    ${KODI_INCLUDE_DIR}
                    ${RAPIDJSON_INCLUDE_DIRS})

set(DEPLIBS ${p8-platform_LIBRARIES})

set(FREEBOX_SOURCES src/client.cpp
                    src/PVRFreeboxData.cpp)

set(FREEBOX_HEADERS src/client.h
                    src/PVRFreeboxData.h)

build_addon(pvr.freebox FREEBOX DEPLIBS)

include(CPack)
