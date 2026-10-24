# xbox-cast media: Linux test harness for the portable media code (NOT part of the UWP build).
#   cmake -S app/src/media/test -B build/media-test && cmake --build build/media-test -j
#   build/media-test/media_tests            (generates fixtures with the ffmpeg CLI on first run)
# Needs: gcc/clang (C++17), pkg-config, libavcodec/libavformat/libavutil/libswresample dev packages, ffmpeg CLI with
# libx264 + libx265.
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(xboxcast_media_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FF REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswresample)
find_package(Threads REQUIRED)

set(PORTABLE ${CMAKE_CURRENT_SOURCE_DIR}/../portable)
file(GLOB PORTABLE_SRC ${PORTABLE}/*.cpp)

add_executable(media_tests
    main.cc
    test_math.cc
    test_crop.cc
    test_decode.cc
    test_audio_sim.cc
    ${PORTABLE_SRC})
# pch.h here is an empty stand-in for the app's precompiled header
target_include_directories(media_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PORTABLE})
target_compile_options(media_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(media_tests PRIVATE PkgConfig::FF Threads::Threads m)

set(FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
add_custom_target(fixtures
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/make_fixtures.sh ${FIXTURES}
    COMMENT "Generating media fixtures with ffmpeg")
add_dependencies(media_tests fixtures)
target_compile_definitions(media_tests PRIVATE XC_DEFAULT_FIXTURES="${FIXTURES}")

enable_testing()
add_test(NAME media_tests COMMAND media_tests --fixtures ${FIXTURES})
