option(TEEZ_ENABLE_COVERAGE "Build with gcov coverage instrumentation" OFF)

function(_teez_strip_coverage_from_flags in_var out_var)
    string(REGEX REPLACE "(^| )--coverage( |$)" " " _val "${in_var}")
    string(REGEX REPLACE "(^| )-fprofile-arcs( |$)" " " _val "${_val}")
    string(REGEX REPLACE "(^| )-ftest-coverage( |$)" " " _val "${_val}")
    string(STRIP "${_val}" _val)
    set(${out_var} "${_val}" PARENT_SCOPE)
endfunction()

function(_teez_strip_coverage_flags)
    set(_had_coverage FALSE)

    get_directory_property(_compile_opts COMPILE_OPTIONS)
    if(_compile_opts)
        foreach(_opt IN LISTS _compile_opts)
            if(_opt MATCHES "^(--coverage|-fprofile-arcs|-ftest-coverage)$")
                set(_had_coverage TRUE)
                break()
            endif()
        endforeach()
        list(FILTER _compile_opts EXCLUDE REGEX "^(--coverage|-fprofile-arcs|-ftest-coverage)$")
        set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_compile_opts}")
    endif()

    get_directory_property(_link_opts LINK_OPTIONS)
    if(_link_opts)
        foreach(_opt IN LISTS _link_opts)
            if(_opt STREQUAL "--coverage")
                set(_had_coverage TRUE)
                break()
            endif()
        endforeach()
        list(FILTER _link_opts EXCLUDE REGEX "^--coverage$")
        set_directory_properties(PROPERTIES LINK_OPTIONS "${_link_opts}")
    endif()

    foreach(_var CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS CMAKE_MODULE_LINKER_FLAGS)
        if(DEFINED ${_var} AND "${${_var}}" MATCHES "(^| )--coverage( |$)")
            set(_had_coverage TRUE)
            _teez_strip_coverage_from_flags("${${_var}}" _stripped)
            set(${_var} "${_stripped}" CACHE STRING "" FORCE)
        endif()
    endforeach()

    if(_had_coverage)
        file(WRITE "${CMAKE_BINARY_DIR}/.teez_coverage_stripped" "")
    endif()
endfunction()

if(NOT TEEZ_ENABLE_COVERAGE)
    _teez_strip_coverage_flags()
    execute_process(
        COMMAND find "${CMAKE_BINARY_DIR}" -name "*.gcda" -type f -delete
        RESULT_VARIABLE _teez_gcda_cleanup_status
        ERROR_QUIET
    )
    if(_teez_gcda_cleanup_status EQUAL 0)
        message(STATUS "${PROJECT_NAME}: removed stale gcov profile data (coverage disabled)")
    endif()
endif()

if(TEEZ_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(STATUS "${PROJECT_NAME}: coverage instrumentation enabled")
        add_compile_options(-O0 -g --coverage)
        add_link_options(--coverage)

        if(NOT TARGET teez_gcov_prebuild)
            add_custom_target(
                teez_gcov_prebuild
                COMMAND ${CMAKE_COMMAND} -E echo "Removing stale gcov profile data in ${CMAKE_BINARY_DIR}"
                COMMAND find "${CMAKE_BINARY_DIR}" -name '*.gcda' -delete
                COMMENT "Sanitizing gcov profile data before build"
                VERBATIM
            )
        endif()

        if(NOT TARGET "${PROJECT_NAME}-clean-coverage-data")
            add_custom_target(
                "${PROJECT_NAME}-clean-coverage-data"
                COMMAND find "${CMAKE_BINARY_DIR}" -name '*.gcda' -delete
                COMMENT "Removing gcov profile data from ${PROJECT_NAME} build tree"
                VERBATIM
            )
        endif()
    else()
        message(WARNING "TEEZ_ENABLE_COVERAGE is not supported for ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

function(teez_collect_object_directories out_var)
    set(_dirs "${CMAKE_BINARY_DIR}")
    if(EXISTS "${CMAKE_BINARY_DIR}/teez-core-build")
        list(APPEND _dirs "${CMAKE_BINARY_DIR}/teez-core-build")
    endif()
    set(${out_var} "${_dirs}" PARENT_SCOPE)
endfunction()

function(teez_apply_gcov_prebuild_cleanup)
    if(NOT TEEZ_ENABLE_COVERAGE OR NOT TARGET teez_gcov_prebuild)
        return()
    endif()

    get_property(_targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        if(NOT TARGET ${_target})
            continue()
        endif()
        get_target_property(_type ${_target} TYPE)
        if(_type STREQUAL "EXECUTABLE")
            add_dependencies(${_target} teez_gcov_prebuild)
        endif()
    endforeach()
endfunction()

function(teez_add_coverage_target target_name project_label)
    if(NOT TEEZ_ENABLE_COVERAGE)
        return()
    endif()

    find_program(TEEZ_GCOVR_EXECUTABLE gcovr)
    if(NOT TEEZ_GCOVR_EXECUTABLE)
        message(WARNING "${PROJECT_NAME}: gcovr not found; install gcovr to generate coverage reports")
        return()
    endif()

    teez_collect_object_directories(_object_dirs)
    set(_output_dir "${CMAKE_BINARY_DIR}/coverage")
    set(_summary "${_output_dir}/summary.txt")

    set(_gcovr_cmd "${TEEZ_GCOVR_EXECUTABLE}")
    foreach(_dir IN LISTS _object_dirs)
        list(APPEND _gcovr_cmd --object-directory "${_dir}")
    endforeach()
    list(APPEND _gcovr_cmd
        --root "${CMAKE_CURRENT_SOURCE_DIR}"
        --filter "${CMAKE_CURRENT_SOURCE_DIR}/src"
        --filter "${CMAKE_CURRENT_SOURCE_DIR}/include"
        --exclude '.*/tests/.*'
        --exclude '.*/_deps/.*'
        --exclude '.*/build/.*'
        --exclude '.*/CMakeFiles/.*'
        --gcov-ignore-errors=source_not_found
        --html-details "${_output_dir}/index.html"
        --txt "${_summary}"
        --print-summary
    )

    add_custom_target(
        ${target_name}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_output_dir}"
        COMMAND ${_gcovr_cmd}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Generating ${project_label} coverage report"
        VERBATIM
    )
endfunction()
