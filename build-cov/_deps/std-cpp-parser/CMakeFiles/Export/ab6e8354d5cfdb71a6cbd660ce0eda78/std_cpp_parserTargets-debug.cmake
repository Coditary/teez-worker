#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "coditary::std_cpp_parser_core" for configuration "Debug"
set_property(TARGET coditary::std_cpp_parser_core APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(coditary::std_cpp_parser_core PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libstd_cpp_parser_core.a"
  )

list(APPEND _cmake_import_check_targets coditary::std_cpp_parser_core )
list(APPEND _cmake_import_check_files_for_coditary::std_cpp_parser_core "${_IMPORT_PREFIX}/lib/libstd_cpp_parser_core.a" )

# Import target "coditary::std_cpp_parser_json" for configuration "Debug"
set_property(TARGET coditary::std_cpp_parser_json APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(coditary::std_cpp_parser_json PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libstd_cpp_parser_json.a"
  )

list(APPEND _cmake_import_check_targets coditary::std_cpp_parser_json )
list(APPEND _cmake_import_check_files_for_coditary::std_cpp_parser_json "${_IMPORT_PREFIX}/lib/libstd_cpp_parser_json.a" )

# Import target "coditary::std_cpp_parser_codec" for configuration "Debug"
set_property(TARGET coditary::std_cpp_parser_codec APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(coditary::std_cpp_parser_codec PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libstd_cpp_parser_codec.a"
  )

list(APPEND _cmake_import_check_targets coditary::std_cpp_parser_codec )
list(APPEND _cmake_import_check_files_for_coditary::std_cpp_parser_codec "${_IMPORT_PREFIX}/lib/libstd_cpp_parser_codec.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
