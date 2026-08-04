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
## A plain text substitution run via CMake's own string()/file() commands,
## not `git apply` against a checked-in .patch file. This project's first
## attempt at this fix used `git apply` and it did not take effect on the
## real Windows CI runner, for a reason that could not be pinned down after
## it was checked (a simulated CRLF-line-ending checkout still let `git
## apply --ignore-whitespace` succeed, just messily -- so line endings alone
## don't explain it). Rather than keep guessing at git's behaviour on a
## runner this project can't directly inspect, this version sidesteps the
## whole class of problem: string(FIND)/string(REPLACE) match on file
## content directly and don't care about line endings, and this script is
## unambiguous about what happened -- every outcome is a message() the
## Configure log will show, not something to infer from whether a build
## error later did or didn't reappear.
##
## Idempotent by design (checks for its own marker before touching a file),
## so re-running configure against an already-populated, already-patched
## source tree is a no-op rather than a double-patch. Fails LOUDLY via
## FATAL_ERROR if a target file doesn't exist or the expected anchor line
## isn't found in it -- almost certainly because KAMIFRAME_LVGL_TAG in
## fetch_lvgl.cmake was bumped to a new LVGL release and this script needs
## updating for it, not something to let slide silently into another
## unpatched, MSVC-broken build.
##
## Runs with LVGL's source directory as the working directory -- this is how
## CMake's PATCH_COMMAND step already worked before this rewrite, unchanged.

function(kf_lvgl_patch_bitfields relative_path anchor insertion marker)
    if(NOT EXISTS "${relative_path}")
        message(FATAL_ERROR
            "LVGL MSVC fix: expected file '${relative_path}' does not exist "
            "in the fetched LVGL source. LVGL's layout has changed (a "
            "KAMIFRAME_LVGL_TAG bump in cmake/fetch_lvgl.cmake?) -- "
            "cmake/patches/lvgl_msvc_c2099_fix.cmake needs updating to match.")
    endif()

    file(READ "${relative_path}" file_content)

    if(file_content MATCHES "${marker}")
        message(STATUS "LVGL MSVC fix: ${relative_path} already patched, skipping")
        return()
    endif()

    string(FIND "${file_content}" "${anchor}" anchor_pos)
    if(anchor_pos EQUAL -1)
        message(FATAL_ERROR
            "LVGL MSVC fix: expected anchor line not found in "
            "'${relative_path}'. LVGL's source no longer matches what this "
            "script expects (a KAMIFRAME_LVGL_TAG bump?) -- "
            "cmake/patches/lvgl_msvc_c2099_fix.cmake needs updating for "
            "whatever changed, rather than silently shipping an unpatched, "
            "MSVC-broken LVGL again.")
    endif()

    string(REPLACE "${anchor}" "${anchor}\n${insertion}" patched_content "${file_content}")
    file(WRITE "${relative_path}" "${patched_content}")
    message(STATUS "LVGL MSVC fix: patched ${relative_path}")
endfunction()

kf_lvgl_patch_bitfields(
    "src/widgets/bar/lv_bar.c"
    "    .instance_size = sizeof(lv_bar_t),"
    "    .editable = LV_OBJ_CLASS_EDITABLE_INHERIT,\n    .group_def = LV_OBJ_CLASS_GROUP_DEF_INHERIT,\n    .theme_inheritable = LV_OBJ_CLASS_THEME_INHERITABLE_FALSE,"
    "LV_OBJ_CLASS_EDITABLE_INHERIT"
)

kf_lvgl_patch_bitfields(
    "src/widgets/button/lv_button.c"
    "    .instance_size = sizeof(lv_button_t),"
    "    .editable = LV_OBJ_CLASS_EDITABLE_INHERIT,\n    .theme_inheritable = LV_OBJ_CLASS_THEME_INHERITABLE_FALSE,"
    "LV_OBJ_CLASS_EDITABLE_INHERIT"
)

kf_lvgl_patch_bitfields(
    "src/widgets/image/lv_image.c"
    "    .instance_size = sizeof(lv_image_t),"
    "    .editable = LV_OBJ_CLASS_EDITABLE_INHERIT,\n    .group_def = LV_OBJ_CLASS_GROUP_DEF_INHERIT,\n    .theme_inheritable = LV_OBJ_CLASS_THEME_INHERITABLE_FALSE,"
    "LV_OBJ_CLASS_EDITABLE_INHERIT"
)

kf_lvgl_patch_bitfields(
    "src/widgets/label/lv_label.c"
    "    .instance_size = sizeof(lv_label_t),"
    "    .editable = LV_OBJ_CLASS_EDITABLE_INHERIT,\n    .group_def = LV_OBJ_CLASS_GROUP_DEF_INHERIT,\n    .theme_inheritable = LV_OBJ_CLASS_THEME_INHERITABLE_FALSE,"
    "LV_OBJ_CLASS_EDITABLE_INHERIT"
)
