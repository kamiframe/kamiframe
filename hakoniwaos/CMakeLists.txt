## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## HakoniwaOS core.
##
## ONE file, TWO build systems. ESP-IDF defines ESP_PLATFORM, so the same
## directory is a normal CMake static library on the desktop and an ESP-IDF
## component on the device, with the source list shared between them.
##
## Note the directory name: `hakoniwaos`, never bare `hakoniwa`. ESP-IDF takes
## the component name straight from the directory name, and it must be unique
## against every component inside ESP-IDF itself, so this is one of the places
## the naming rule is load-bearing rather than cosmetic.

include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

if(ESP_PLATFORM)

    idf_component_register(
        SRCS ${HAKONIWAOS_SRCS}
        INCLUDE_DIRS include
        REQUIRES esp_timer
    )
    # ESP-IDF defaults its own components to gnu++26. Pin ours to the same
    # standard the desktop build uses, so the two cannot diverge.
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    )

else()

    add_library(hakoniwaos STATIC ${HAKONIWAOS_SRCS})
    add_library(kamiframe::hakoniwaos ALIAS hakoniwaos)

    target_include_directories(hakoniwaos
        PUBLIC  ${CMAKE_CURRENT_LIST_DIR}/include
    )

    target_link_libraries(hakoniwaos PUBLIC kamiframe_options)

    # ------------------------------------------------------------------
    # The HAL boundary is checked, not just documented.
    #
    # Every header under include/kf/hal/ must compile as plain C17. That is a
    # mechanical test of "this interface is simple": a header a C compiler
    # accepts cannot contain templates, overloads, default arguments or
    # classes with behaviour. It also means a backend can be written in C.
    #
    # Fails the build if someone widens the boundary by accident.
    # ------------------------------------------------------------------
    file(GLOB KF_HAL_HEADERS "${CMAKE_CURRENT_LIST_DIR}/include/kf/hal/*.h")
    set(KF_C_CHECK "${CMAKE_CURRENT_BINARY_DIR}/hal_c_compat_check.c")
    set(KF_C_CHECK_BODY "/* GENERATED. Proves the HAL headers are valid C. */\n")
    foreach(header ${KF_HAL_HEADERS})
        get_filename_component(name "${header}" NAME)
        string(APPEND KF_C_CHECK_BODY "#include \"kf/hal/${name}\"\n")
    endforeach()
    string(APPEND KF_C_CHECK_BODY
        "#include \"kf/types.h\"\n"
        "#include \"kf/budget.h\"\n"
        "#include \"kf/arena.h\"\n"
        "#include \"kf/app.h\"\n"
        "int kf_hal_c_compat_check(void) { return KF_HAL_DISPLAY_VERSION; }\n")
    file(GENERATE OUTPUT "${KF_C_CHECK}" CONTENT "${KF_C_CHECK_BODY}")

    add_library(hakoniwaos_hal_c_check STATIC "${KF_C_CHECK}")
    set_target_properties(hakoniwaos_hal_c_check PROPERTIES
        C_STANDARD 17
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    target_include_directories(hakoniwaos_hal_c_check
        PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)

endif()
