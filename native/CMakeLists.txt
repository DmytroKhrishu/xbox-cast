# castcore: AirPlay mirroring + audio receiver library of xbox-cast (GPL-3.0-or-later).
#
#   Linux:  cmake -S native -B build && cmake --build build      -> castcore (static), hostrx, unit tests
#   UWP:    pwsh native/build-uwp.ps1 -Arch x64 -Config Release  -> native/out/uwp-x64 (docs/contract.md section 2)
cmake_minimum_required(VERSION 3.21)
project(castcore C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(CASTCORE_BUILD_HOSTRX "Build the Linux test receiver (needs avahi-client + FFmpeg)" ON)
option(CASTCORE_BUILD_TESTS "Build the unit tests" ON)

set(UXPLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/uxplay/lib)

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    set(XC_UWP ON)
endif()

# ---------------------------------------------------------------- dependencies
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

if(WIN32)
    # vcpkg port "libplist" (static, LGPL-2.1-or-later)
    find_package(unofficial-libplist CONFIG REQUIRED)
    set(XC_PLIST_TARGET unofficial::libplist::libplist)
    set(XC_PLIST_VERSION 2.8.0)
else()
    find_package(PkgConfig REQUIRED)
    pkg_search_module(PLIST REQUIRED IMPORTED_TARGET libplist-2.0 libplist>=2.0)
    set(XC_PLIST_TARGET PkgConfig::PLIST)
    set(XC_PLIST_VERSION ${PLIST_VERSION})
    find_package(Threads REQUIRED)
endif()

# ---------------------------------------------------------------- UxPlay protocol library (vendored)
file(GLOB UXPLAY_SOURCES
    ${UXPLAY_DIR}/*.c
    ${UXPLAY_DIR}/mdnsd/*.c
    ${UXPLAY_DIR}/llhttp/*.c
    ${UXPLAY_DIR}/playfair/*.c)

add_library(castcore STATIC
    src/castcore.c
    src/h264_sps.c
    ${UXPLAY_SOURCES})

target_include_directories(castcore
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${UXPLAY_DIR} ${UXPLAY_DIR}/llhttp ${UXPLAY_DIR}/playfair)

target_compile_definitions(castcore PRIVATE OPENSSL_API_COMPAT=0x10101000L)
if(XC_PLIST_VERSION VERSION_GREATER_EQUAL 2.1.0)
    target_compile_definitions(castcore PRIVATE PLIST_210)
endif()
if(XC_PLIST_VERSION VERSION_GREATER_EQUAL 2.3.0)
    target_compile_definitions(castcore PRIVATE PLIST_230)
endif()

target_link_libraries(castcore PRIVATE OpenSSL::Crypto ${XC_PLIST_TARGET})

if(MSVC)
    # UxPlay is POSIX C: pthread/unistd shims + a force-included header (ssize_t, strcasecmp)
    target_include_directories(castcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/compat/msvc)
    target_compile_options(castcore PRIVATE
        /FI${CMAKE_CURRENT_SOURCE_DIR}/src/compat/msvc/xc_msvc_force.h
        /utf-8 /W3
        /wd4996   # POSIX names (strdup, fopen, ...)
        /wd4244 /wd4267 /wd4018 /wd4146 /wd4334 /wd4005 /wd4013)
    target_compile_definitions(castcore PRIVATE
        WIN32 _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX
        _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS LIBPLIST_STATIC)
    if(XC_UWP)
        # Ninja does not set the app partition the way the VS generator does (same flags as vcpkg's uwp toolchain)
        target_compile_definitions(castcore PRIVATE XC_UWP WINAPI_FAMILY=WINAPI_FAMILY_APP __WRL_NO_DEFAULT_LIB__)
    endif()
    target_link_libraries(castcore PRIVATE ws2_32 iphlpapi bcrypt)
else()
    target_compile_definitions(castcore PRIVATE _GNU_SOURCE)
    target_compile_options(castcore PRIVATE -Wall -Wno-unused-function)
    # UxPlay itself is not warning-clean; keep our own sources strict
    set_source_files_properties(src/castcore.c src/h264_sps.c PROPERTIES COMPILE_OPTIONS "-Wextra;-Wno-unused-parameter;-Wvla")
    target_link_libraries(castcore PRIVATE Threads::Threads m)
endif()

install(TARGETS castcore ARCHIVE DESTINATION lib)
install(FILES include/castcore.h DESTINATION include)

# UWP link check: a DLL that references the whole public API, linked like the app links castcore
# (castcore.lib + the vcpkg static libs + system import libs, /APPCONTAINER, WindowsApp.lib). Proves there are no
# unresolved externals and no API outside the app partition before the platform's app build sees the libs.
if(XC_UWP)
    add_library(castcore_linkcheck SHARED tests/linkcheck_uwp.c)
    target_compile_definitions(castcore_linkcheck PRIVATE WINAPI_FAMILY=WINAPI_FAMILY_APP)
    target_link_libraries(castcore_linkcheck PRIVATE castcore)
    target_link_options(castcore_linkcheck PRIVATE /APPCONTAINER /MANIFEST:NO /WINMD:NO)
endif()

# ---------------------------------------------------------------- Linux tools
if(NOT WIN32)
    if(CASTCORE_BUILD_TESTS)
        enable_testing()
        pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
        add_executable(test_h264_sps tests/test_h264_sps.c src/h264_sps.c)
        target_include_directories(test_h264_sps PRIVATE src tests)
        target_link_libraries(test_h264_sps PRIVATE PkgConfig::FFMPEG)
        add_test(NAME h264_sps COMMAND test_h264_sps)

        pkg_check_modules(LAVF REQUIRED IMPORTED_TARGET libavformat)
        add_executable(test_aac_eld tests/test_aac_eld.c)
        target_link_libraries(test_aac_eld PRIVATE PkgConfig::FFMPEG PkgConfig::LAVF)
        # decodes a real AAC-ELD stream when XC_ELD_SAMPLE points at FFmpeg's FATE aac/er_eld1001np_44_ep0.mp4
        add_test(NAME aac_eld COMMAND test_aac_eld)

        add_executable(test_castcore tests/test_castcore.c)
        target_compile_definitions(test_castcore PRIVATE _GNU_SOURCE)
        target_link_libraries(test_castcore PRIVATE castcore OpenSSL::Crypto ${XC_PLIST_TARGET} Threads::Threads)
        add_test(NAME castcore_info COMMAND test_castcore)
    endif()

    if(CASTCORE_BUILD_HOSTRX)
        pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)
        if(NOT TARGET PkgConfig::FFMPEG)
            pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
        endif()
        add_executable(hostrx hostrx/hostrx.c)
        target_compile_definitions(hostrx PRIVATE _GNU_SOURCE)
        target_compile_options(hostrx PRIVATE -Wall -Wextra -Wno-unused-parameter)
        target_link_libraries(hostrx PRIVATE castcore PkgConfig::AVAHI PkgConfig::FFMPEG Threads::Threads)
    endif()
endif()
