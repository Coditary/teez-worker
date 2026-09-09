function(coditary_resolve_net)
    if(TARGET coditary_net)
        return()
    endif()

    if(NOT CODITARY_NET_DIR)
        set(_net_candidates
            "${CMAKE_CURRENT_SOURCE_DIR}/Shared-Cpp/coditary_net"
        )
        if(TEEZ_CORE_SOURCE_DIR)
            list(APPEND _net_candidates "${TEEZ_CORE_SOURCE_DIR}/Shared-Cpp/coditary_net")
        endif()
        list(APPEND _net_candidates
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../Coditary/shared/Shared-Cpp/coditary_net"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../shared/Shared-Cpp/coditary_net"
            "${CMAKE_CURRENT_SOURCE_DIR}/../Shared-Cpp/coditary_net"
        )
        foreach(candidate IN LISTS _net_candidates)
            if(EXISTS "${candidate}/CMakeLists.txt")
                set(CODITARY_NET_DIR "${candidate}" CACHE PATH "Path to coditary_net" FORCE)
                break()
            endif()
        endforeach()
    endif()

    if(NOT CODITARY_NET_DIR OR NOT EXISTS "${CODITARY_NET_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "coditary_net not found. Set -DCODITARY_NET_DIR=... to Shared-Cpp/coditary_net")
    endif()

    add_subdirectory("${CODITARY_NET_DIR}" "${CMAKE_BINARY_DIR}/_deps/coditary_net")
endfunction()
