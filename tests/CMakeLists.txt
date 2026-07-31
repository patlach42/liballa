cmake_minimum_required(VERSION 3.22)

# Library contract tests are opt-in and reuse the existing GoogleTest release.
option(LOWLATENCYAUDIO_BUILD_TESTS "Build liblowlatencyaudio tests" OFF)
if(NOT LOWLATENCYAUDIO_BUILD_TESTS)
    return()
endif()

include(FetchContent)
if(NOT TARGET GTest::gtest_main)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)
enable_testing()

add_executable(usb_scheduling_tests TestUsbScheduling.cpp)
target_compile_features(usb_scheduling_tests PRIVATE cxx_std_17)
target_link_libraries(usb_scheduling_tests PRIVATE
    lowlatencyaudio::lowlatencyaudio
    GTest::gtest_main)
gtest_discover_tests(usb_scheduling_tests)

if(NOT ANDROID)
    # The driver tests mock transfer submission but use the host libusb ABI.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
    endif()
    if(TARGET PkgConfig::LIBUSB)
        add_executable(usb_driver_tests UsbDriverTests.cpp MockLibusbApi.cpp)
        target_compile_features(usb_driver_tests PRIVATE cxx_std_17)
        target_link_libraries(usb_driver_tests PRIVATE
            lowlatencyaudio::lowlatencyaudio
            GTest::gtest_main
            PkgConfig::LIBUSB
            pthread)
        gtest_discover_tests(usb_driver_tests)
    else()
        message(STATUS "USB driver tests: DISABLED (host libusb-1.0 not found)")
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(audio_cpu_affinity_tests TestAudioCpuAffinity.cpp)
    target_compile_features(audio_cpu_affinity_tests PRIVATE cxx_std_17)
    target_link_libraries(audio_cpu_affinity_tests PRIVATE
        lowlatencyaudio::lowlatencyaudio
        GTest::gtest_main
        pthread)
    gtest_discover_tests(audio_cpu_affinity_tests)
endif()
