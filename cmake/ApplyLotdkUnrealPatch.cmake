# Keep the pinned upstream dependency and carry the LOTDK change in this repo.
# Reconfiguration accepts an already-applied patch; divergent files fail visibly.
find_package(Git REQUIRED)
if(NOT DEFINED LOTDK_UNREAL_SOURCE_DIR)
    get_filename_component(LOTDK_UNREAL_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../deps/first/Unreal" ABSOLUTE)
endif()
get_filename_component(_lotdk_hook_patch "${CMAKE_CURRENT_LIST_DIR}/../patches/unreal-hook-diagnostics.patch" ABSOLUTE)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${LOTDK_UNREAL_SOURCE_DIR}" apply --check --ignore-space-change "${_lotdk_hook_patch}"
    RESULT_VARIABLE _lotdk_can_apply OUTPUT_QUIET ERROR_QUIET)
if(_lotdk_can_apply EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${LOTDK_UNREAL_SOURCE_DIR}" apply --ignore-space-change "${_lotdk_hook_patch}"
        RESULT_VARIABLE _lotdk_applied ERROR_VARIABLE _lotdk_error)
    if(NOT _lotdk_applied EQUAL 0)
        message(FATAL_ERROR "Could not apply LOTDK Unreal hook patch: ${_lotdk_error}")
    endif()
    message(STATUS "Applied LOTDK Unreal hook diagnostics patch")
else()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${LOTDK_UNREAL_SOURCE_DIR}" apply --reverse --check --ignore-space-change "${_lotdk_hook_patch}"
        RESULT_VARIABLE _lotdk_already_applied OUTPUT_QUIET ERROR_VARIABLE _lotdk_error)
    if(NOT _lotdk_already_applied EQUAL 0)
        message(FATAL_ERROR "Unreal hook patch neither applies nor matches the working files. Check the pinned submodule and local edits: ${_lotdk_error}")
    endif()
    message(STATUS "LOTDK Unreal hook diagnostics patch already applied")
endif()
