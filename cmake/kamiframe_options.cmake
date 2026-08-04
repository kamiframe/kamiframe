## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## One interface target carrying the compile settings every Kamiframe target
## uses. Link it and you get the same language rules the device build has.
##
## The important part is what is switched OFF.
##
## ESP-IDF disables C++ exceptions and RTTI by default, because both cost
## binary size and unpredictable latency on a microcontroller. If the desktop
## build left them on, you would write error handling for months that simply
## does not compile for the device. So they are off here too, from the first
## commit, and the problem never exists.
##
## Same reasoning as capping the framebuffer at 240x320: match the device now,
## not later.

add_library(kamiframe_options INTERFACE)

target_compile_features(kamiframe_options INTERFACE cxx_std_17)

if(MSVC)
    target_compile_options(kamiframe_options INTERFACE
        /W4
        /permissive-        # standards conformance
        /EHs-c-             # no C++ exceptions
        /GR-                # no RTTI
        /utf-8
        /Zc:__cplusplus
    )
    target_compile_definitions(kamiframe_options INTERFACE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    if(KAMIFRAME_WARNINGS_AS_ERRORS)
        target_compile_options(kamiframe_options INTERFACE /WX)
    endif()
else()
    target_compile_options(kamiframe_options INTERFACE
        -Wall
        -Wextra
        -Wshadow
        -Wconversion            # catches the RGB565 packing mistakes
        -Wsign-conversion
        -Wcast-qual
        -Wdouble-promotion      # the S3's FPU is single-precision only
        -Wno-unused-parameter
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    )
    if(KAMIFRAME_WARNINGS_AS_ERRORS)
        target_compile_options(kamiframe_options INTERFACE -Werror)
    endif()
endif()
