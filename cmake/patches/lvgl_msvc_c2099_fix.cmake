## SPDX-License-Identifier: Apache-2.0
## Copyright the Kamiframe contributors.
##
## MSVC-only fix, see docs/architecture/adr-0013-lvgl-for-menus.md for the
## full explanation: MSVC's C11 compiler cannot constant-fold
## lv_obj_class_t's packed bit-field word (editable / group_def /
## instance_size / theme_inheritable) when a widget's designated initializer
## only sets some of those bit-fields and leaves the rest to their implicit
## zero default -- `error C2099: initializer is not a constant`, in
## lv_bar.c, lv_button.c, lv_image.c and lv_label.c. This makes the omitted
## bit-fields explicit, to the SAME zero values they already defaulted to --
## a semantic no-op on every compiler, GCC/Clang included -- so MSVC has a
## complete word to fold.
##
## include()'d directly from fetch_lvgl.cmake, right after
## FetchContent_MakeAvailable(lvgl) -- deliberately NOT wired up as a
## FetchContent PATCH_COMMAND. Two earlier versions of this fix were: first
## a `git apply` of a checked-in .patch file, then a `cmake -P` of an
## equivalent version of this same script, both run via PATCH_COMMAND. Both
## were verified working, repeatedly, in this project's own sandbox -- and
## both still failed identically on the real Windows CI runner (same four
## `error C2099`s, same lines, as if nothing had run at all). Since two
## unrelated patch *mechanisms* (an external `git` process, and CMake's own
## `-P` script mode) failed the same way, the common factor isn't the patch
## content or the tool -- it's PATCH_COMMAND itself, which only executes
## inside FetchContent's separate populate "subbuild", a mechanism with real
## generator- and CMake-version-dependent execution behavior (see
## CMP0168 and the OLD/NEW FetchContent Git download split introduced in
## CMake 3.30) that could not be reproduced or root-caused without an actual
## Windows + Visual Studio generator machine to test against.
##
## This version has no PATCH_COMMAND dependency at all: kf_lvgl_apply_msvc_fix()
## below is called as ordinary CMake script code, in the same configure pass
## as everything else in fetch_lvgl.cmake, operating on lvgl_SOURCE_DIR (the
## variable FetchContent_MakeAvailable() already sets). No subbuild, no
## separate step execution, nothing generator-dependent to fail silently on
## again.
##
## Idempotent by design (checks for its own marker before touching a file),
## so it is safe to run on every configure -- including every reconfigure
## against an already-populated, already-patched source tree, which it now
## always does (this used to only matter for a first-time PATCH_COMMAND
## population; it runs unconditionally now, which is more robust, not less:
## correctness gets re-checked every configure instead of assumed after the
## first one). Fails LOUDLY via FATAL_ERROR if a target file doesn't exist or
## the expected anchor line isn't found in it -- almost certainly because
## KAMIFRAME_LVGL_TAG in fetch_lvgl.cmake was bumped to a new LVGL release
## and this script needs updating for it, not something to let slide
## silently into another unpatched, MSVC-broken build.

function(kf_lvgl_patch_bitfields absolute_path anchor insertion marker)
    if(NOT EXISTS "${absolute_path}")
        message(FATAL_ERROR
            "LVGL MSVC fix: expected file '${absolute_path}' does not exist "
            "in the fetched LVGL source. LVGL's layout has changed (a "
            "KAMIFRAME_LVGL_TAG bump in cmake/fetch_lvgl.cmake?) -- "
            "cmake/patches/lvgl_msvc_c2099_fix.cmake needs updating to match.")
    endif()

    file(READ "${absolute_path}" file_content)

    if(file_content MATCHES "${marker}")
        message(STATUS "LVGL MSVC fix: ${absolute_path} already patched, skipping")
        return()
    endif()

    string(FIND "${file_content}" "${anchor}" anchor_pos)
    if(anchor_pos EQUAL -1)
        message(FATAL_ERROR
            "LVGL MSVC fix: expected anchor line not found in "
            "'${absolute_path}'. LVGL's source no longer matches what this "
            "script expects (a KAMIFRAME_LVGL_TAG bump?) -- "
            "cmake/patches/lvgl_msvc_c2099_fix.cmake needs updating for "
            "whatever changed, rather than silently shipping an unpatched, "
            "MSVC-broken LVGL again.")
    endif()

    string(REPLACE "${anchor}" "${anchor}\n${insertion}" patched_content "${file_content}")
    file(WRITE "${absolute_path}" "${patched_content}")
    message(STATUS "LVGL MSVC fix: patched ${absolute_path}")
endfunction()

function(kf_lvgl_apply_msvc_fix lvgl_source_dir)
    kf_lvgl_patch_bitfields(
        "${lvgl_source_dir}/src/widgets/bar/lv_bar.c"
        "    .instance_size = sizeof(lv_bar_t),"
        "    .editable = LV_OBJ_CLASS_EDITABLE_INHERIT,\n    .group_def = LV_OBJ_CLASS_GROUP_DEF_INHERIT,\n    .theme_inheritable = LV_OBJ_CLASS_THEME_INHERITABLE_FALSE,"
        "LV_OBJ_CLASS_EDITABLE_INHERIT"
    )

    kf_lvgl_patch_bitfields(
        "${lvgl_source_dir}/src/widgets/button/lv_button.c"
        "    .instance_size = sizeof(lv_button_t),"
        "    .editable = LV_OBJ_CLASS_EDITABLE_INHERIT,\n    .theme_inheritable = LV_OBJ_CLASS_THEME_INHERITABLE_FALSE,"
        "LV_OBJ_CLASS_EDITABLE_INHERIT"
    )

    kf_lvgl_patch_bitfields(
        "${lvgl_source_dir}/src/widgets/image/lv_image.c"
        "    .instance_size = sizeof(lv_image_t),"
        "    .editable = LV_OBJ_CLASS_EDITABLE_INHERIT,\n    .group_def = LV_OBJ_CLASS_GROUP_DEF_INHERIT,\n    .theme_inheritable = LV_OBJ_CLASS_THEME_INHERITABLE_FALSE,"
        "LV_OBJ_CLASS_EDITABLE_INHERIT"
    )

    kf_lvgl_patch_bitfields(
        "${lvgl_source_dir}/src/widgets/label/lv_label.c"
        "    .instance_size = sizeof(lv_label_t),"
        "    .editable = LV_OBJ_CLASS_EDITABLE_INHERIT,\n    .group_def = LV_OBJ_CLASS_GROUP_DEF_INHERIT,\n    .theme_inheritable = LV_OBJ_CLASS_THEME_INHERITABLE_FALSE,"
        "LV_OBJ_CLASS_EDITABLE_INHERIT"
    )
endfunction()
