# Locates teez-core when building teez-worker as a standalone project.
if(TARGET teez_core)
    return()
endif()

include(FetchContent)

set(TEEZ_CORE_DIR "" CACHE PATH "Path to teez-core source tree")
set(TEEZ_CORE_GIT_REPOSITORY "https://github.com/Coditary/teez-core.git" CACHE STRING
    "Git repository used when teez-core is fetched automatically")
set(TEEZ_CORE_GIT_TAG "main" CACHE STRING
    "Git tag or branch used when teez-core is fetched automatically")
set(TEEZ_SHARED_CPP_GIT_REPOSITORY "https://github.com/Coditary/Shared-Cpp.git" CACHE STRING
    "Git repository used when Shared-Cpp is fetched for teez-core")
set(TEEZ_SHARED_CPP_GIT_TAG "main" CACHE STRING
    "Git tag or branch used when Shared-Cpp is fetched for teez-core")
option(TEEZ_FETCH_CORE_IF_MISSING
    "Fetch teez-core and Shared-Cpp from GitHub when not found locally" ON)

function(_teez_ensure_shared_cpp dest_dir)
    if(EXISTS "${dest_dir}/coditary_utils/CMakeLists.txt")
        return()
    endif()
    if(NOT TEEZ_FETCH_CORE_IF_MISSING)
        return()
    endif()

    message(STATUS "Fetching Shared-Cpp into ${dest_dir}")
    FetchContent_Declare(
        teez_shared_cpp
        GIT_REPOSITORY "${TEEZ_SHARED_CPP_GIT_REPOSITORY}"
        GIT_TAG "${TEEZ_SHARED_CPP_GIT_TAG}"
        GIT_SHALLOW TRUE
        SOURCE_DIR "${dest_dir}"
    )
    FetchContent_MakeAvailable(teez_shared_cpp)
endfunction()

if(NOT TEEZ_CORE_DIR)
    set(_default "${CMAKE_CURRENT_LIST_DIR}/../../teez-core")
    if(EXISTS "${_default}/CMakeLists.txt")
        set(TEEZ_CORE_DIR "${_default}" CACHE PATH "Path to teez-core source tree" FORCE)
    endif()
endif()

if(TEEZ_CORE_DIR AND EXISTS "${TEEZ_CORE_DIR}/CMakeLists.txt")
    set(TEEZ_CORE_SOURCE_DIR "${TEEZ_CORE_DIR}")
elseif(TEEZ_FETCH_CORE_IF_MISSING)
    message(STATUS "teez-core not found locally; fetching from ${TEEZ_CORE_GIT_REPOSITORY}")
    FetchContent_Declare(
        teez_core
        GIT_REPOSITORY "${TEEZ_CORE_GIT_REPOSITORY}"
        GIT_TAG "${TEEZ_CORE_GIT_TAG}"
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(teez_core)
    if(NOT teez_core_POPULATED)
        FetchContent_Populate(teez_core)
    endif()
    set(TEEZ_CORE_SOURCE_DIR "${teez_core_SOURCE_DIR}")
else()
    message(FATAL_ERROR
        "teez-core not found.\n"
        "  Clone https://github.com/Coditary/teez-core as a sibling directory,\n"
        "  pass -DTEEZ_CORE_DIR=/path/to/teez-core,\n"
        "  or enable -DTEEZ_FETCH_CORE_IF_MISSING=ON to fetch from GitHub")
endif()

_teez_ensure_shared_cpp("${TEEZ_CORE_SOURCE_DIR}/Shared-Cpp")

add_subdirectory("${TEEZ_CORE_SOURCE_DIR}" teez-core-build EXCLUDE_FROM_ALL)
