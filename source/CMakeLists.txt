cmake_minimum_required(VERSION 3.16)
project(jpg_tool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(HIDAPI_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(HIDAPI_BUILD_HIDTEST OFF CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE)
    set(HIDAPI_WITH_HIDRAW ON CACHE BOOL "" FORCE)
    set(HIDAPI_WITH_LIBUSB OFF CACHE BOOL "" FORCE)
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
if(DEFINED HIDAPI_LOCAL_SRC)
    FetchContent_Declare(hidapi SOURCE_DIR "${HIDAPI_LOCAL_SRC}")
else()
    FetchContent_Declare(hidapi
        URL https://github.com/libusb/hidapi/archive/refs/tags/hidapi-0.14.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
endif()
FetchContent_MakeAvailable(hidapi)

add_library(iap_core
    src/iap_protocol.cpp
    src/iap_client.cpp
)
target_include_directories(iap_core PUBLIC include)
if(WIN32)
    target_link_libraries(iap_core PUBLIC winmm)
endif()

add_library(hid_transport src/hid_transport.cpp)
target_include_directories(hid_transport PUBLIC include)
target_link_libraries(hid_transport PRIVATE hidapi::hidapi)

find_package(Qt5 REQUIRED COMPONENTS Widgets)
add_executable(jpg_tool
    src/qt_jpg_console.cpp
)
target_link_libraries(jpg_tool PRIVATE iap_core hid_transport Qt5::Widgets)
set_target_properties(jpg_tool PROPERTIES AUTOMOC ON)
if(WIN32)
    set_target_properties(jpg_tool PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
