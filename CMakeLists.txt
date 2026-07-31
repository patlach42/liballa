cmake_minimum_required(VERSION 3.22)
project(lowlatencyaudio LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LOWLATENCY_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(ANDROID)
    set(LIBUSB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libusb")
    if(NOT EXISTS "${LIBUSB_ROOT}/libusb/core.c")
        message(FATAL_ERROR
            "Bundled libusb is missing; run: git submodule update --init --recursive")
    endif()
    add_library(lowlatencyaudio_libusb STATIC
        ${LIBUSB_ROOT}/libusb/core.c
        ${LIBUSB_ROOT}/libusb/descriptor.c
        ${LIBUSB_ROOT}/libusb/hotplug.c
        ${LIBUSB_ROOT}/libusb/io.c
        ${LIBUSB_ROOT}/libusb/sync.c
        ${LIBUSB_ROOT}/libusb/strerror.c
        ${LIBUSB_ROOT}/libusb/os/linux_usbfs.c
        ${LIBUSB_ROOT}/libusb/os/events_posix.c
        ${LIBUSB_ROOT}/libusb/os/threads_posix.c
        ${LIBUSB_ROOT}/libusb/os/linux_netlink.c)
    target_include_directories(lowlatencyaudio_libusb PUBLIC
        ${LIBUSB_ROOT}/libusb ${LIBUSB_ROOT}/libusb/os ${LIBUSB_ROOT}/android)
    target_compile_definitions(lowlatencyaudio_libusb PRIVATE LIBUSB_PLATFORM_ANDROID=1)
    target_link_libraries(lowlatencyaudio_libusb PUBLIC log android)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SYSTEM_LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
    endif()
    if(TARGET PkgConfig::SYSTEM_LIBUSB)
        add_library(lowlatencyaudio_libusb INTERFACE)
        target_link_libraries(lowlatencyaudio_libusb INTERFACE PkgConfig::SYSTEM_LIBUSB)
    else()
        find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0 REQUIRED)
        find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0 REQUIRED)
        add_library(lowlatencyaudio_libusb INTERFACE)
        target_include_directories(lowlatencyaudio_libusb INTERFACE ${LIBUSB_INCLUDE_DIR})
        target_link_libraries(lowlatencyaudio_libusb INTERFACE ${LIBUSB_LIBRARY})
    endif()
endif()

add_library(lowlatencyaudio STATIC src/libusb_uac_driver.cpp)
target_include_directories(lowlatencyaudio PUBLIC ${LOWLATENCY_INCLUDE_DIR})
target_link_libraries(lowlatencyaudio PUBLIC lowlatencyaudio_libusb)
set_target_properties(lowlatencyaudio PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(lowlatencyaudio PRIVATE -O3)
add_library(lowlatencyaudio::lowlatencyaudio ALIAS lowlatencyaudio)

option(LOWLATENCYAUDIO_BUILD_TESTS "Build lowlatencyaudio tests" OFF)
if(LOWLATENCYAUDIO_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
