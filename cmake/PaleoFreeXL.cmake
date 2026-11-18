# Keep the XLS runtime next to the executable in a prepared-prefix build.
if(NOT PALEO_DEPENDENCY_PREFIX OR NOT (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
  return()
endif()

set(_pds_freexl_source "${CMAKE_CURRENT_SOURCE_DIR}/vendor/freexl")
get_property(_pds_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
set(_pds_runtime_dir "${CMAKE_BINARY_DIR}")
if(_pds_multi_config)
  string(APPEND _pds_runtime_dir "/$<CONFIG>")
endif()
if(WIN32)
  find_file(_pds_iconv NAMES iconv-2.dll libiconv-2.dll iconv.dll
    PATHS "${PALEO_DEPENDENCY_PREFIX}/bin" NO_DEFAULT_PATH NO_CACHE REQUIRED)
  paleo_require_dependency_path("${_pds_iconv}" "FreeXL iconv runtime")
  get_filename_component(_pds_iconv_name "${_pds_iconv}" NAME)
  set(_pds_charset_args)
  set(_pds_freexl_dlls "${_pds_iconv}")
  if(_pds_iconv_name STREQUAL "iconv.dll")
    find_file(_pds_charset NAMES charset.dll
      PATHS "${PALEO_DEPENDENCY_PREFIX}/bin" NO_DEFAULT_PATH NO_CACHE REQUIRED)
    paleo_require_dependency_path("${_pds_charset}" "FreeXL charset runtime")
    list(APPEND _pds_charset_args -CharsetDll charset.dll)
    list(APPEND _pds_freexl_dlls "${_pds_charset}")
  endif()

  set(_pds_freexl_runtime "${_pds_runtime_dir}/paleo_freexl.dll")
  add_custom_command(OUTPUT "${_pds_freexl_runtime}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${_pds_freexl_source}/build_windows.ps1"
      -OutputDirectory "${_pds_runtime_dir}/freexl-runtime"
      -IconvDll "${_pds_iconv_name}" ${_pds_charset_args}
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${_pds_runtime_dir}/freexl-runtime/freexl.dll" "${_pds_freexl_runtime}"
    DEPENDS "${_pds_freexl_source}/build_windows.ps1"
      "${_pds_freexl_source}/freexl-2.0.0/src/freexl.c"
      "${_pds_freexl_source}/freexl-2.0.0/headers/freexl.h"
      "${_pds_freexl_source}/freexl-2.0.0/headers/freexl_internals.h"
      "${_pds_freexl_source}/freexl-2.0.0/config-msvc.h"
      "${_pds_freexl_source}/windows/iconv.h"
      ${_pds_freexl_dlls}
    VERBATIM)
else()
  find_file(_pds_freexl_library NAMES libfreexl.so.1
    PATHS "${PALEO_DEPENDENCY_PREFIX}/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
          "${PALEO_DEPENDENCY_PREFIX}/lib"
    NO_DEFAULT_PATH NO_CACHE REQUIRED)
  paleo_require_dependency_path("${_pds_freexl_library}" "FreeXL runtime")
  file(REAL_PATH "${_pds_freexl_library}" _pds_freexl_library)
  set(_pds_freexl_runtime "${_pds_runtime_dir}/libpaleo_freexl.so")
  add_custom_command(OUTPUT "${_pds_freexl_runtime}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_pds_runtime_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_pds_freexl_library}" "${_pds_freexl_runtime}"
    DEPENDS "${_pds_freexl_library}"
    VERBATIM)
endif()

add_custom_target(paleo_freexl_runtime DEPENDS "${_pds_freexl_runtime}")
set_property(TARGET paleo_freexl_runtime PROPERTY PALEO_RUNTIME_FILE "${_pds_freexl_runtime}")
add_dependencies(paleo_data_service paleo_freexl_runtime)
set_property(SOURCE src/parsers/auxiliary_parser.cpp APPEND PROPERTY COMPILE_DEFINITIONS PDS_PACKAGED_FREEXL)
if(PDS_INSTALL)
  install(FILES "${_pds_freexl_runtime}" ${_pds_freexl_dlls} DESTINATION bin)
  if(WIN32)
    install(FILES "${_pds_freexl_source}/NOTICE.txt" "${_pds_freexl_source}/LGPL-2.1.txt"
      "${_pds_freexl_source}/spreadsheet_compat.patch" "${_pds_freexl_source}/freexl-2.0.0/COPYING"
      DESTINATION share/paleo/licenses/freexl)
  endif()
endif()
