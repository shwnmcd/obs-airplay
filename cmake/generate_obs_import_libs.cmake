# Generates MinGW .dll.a import libraries directly from the installed OBS Studio DLLs
# so obs-airplay.dll links cleanly against OBS Studio 33's exact obs.dll and FFmpeg 8.0 DLLs.

function(generate_import_lib DLL_NAME OUT_LIB_VAR)
    set(_dll_path "${OBS_STUDIO_BIN_DIR}/${DLL_NAME}.dll")
    if(NOT EXISTS "${_dll_path}")
        message(FATAL_ERROR "Required OBS Studio DLL not found: ${_dll_path}")
    endif()

    set(_imp_dir "${CMAKE_BINARY_DIR}/obs-import-libs")
    file(MAKE_DIRECTORY "${_imp_dir}")

    set(_def_file "${_imp_dir}/${DLL_NAME}.def")
    set(_lib_file "${_imp_dir}/lib${DLL_NAME}.dll.a")

    if(NOT EXISTS "${_lib_file}")
        message(STATUS "Generating import library for ${DLL_NAME}.dll ...")
        execute_process(
            COMMAND "${GENDEF_EXECUTABLE}" - "${_dll_path}"
            OUTPUT_FILE "${_def_file}"
            ERROR_QUIET
            RESULT_VARIABLE _gendef_res
        )
        if(NOT _gendef_res EQUAL 0)
            message(FATAL_ERROR "gendef failed for ${_dll_path} (exit ${_gendef_res})")
        endif()

        execute_process(
            COMMAND "${DLLTOOL_EXECUTABLE}" -d "${_def_file}" -l "${_lib_file}" -D "${DLL_NAME}.dll"
            RESULT_VARIABLE _dlltool_res
        )
        if(NOT _dlltool_res EQUAL 0)
            message(FATAL_ERROR "dlltool failed for ${_def_file} (exit ${_dlltool_res})")
        endif()
    endif()

    set(${OUT_LIB_VAR} "${_lib_file}" PARENT_SCOPE)
endfunction()
