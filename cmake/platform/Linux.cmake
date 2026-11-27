find_program(PATCHELF_EXECUTABLE NAMES patchelf REQUIRED)
set(_ffmpeg_libraries)
foreach(_component IN ITEMS avformat avcodec avfilter avutil swresample)
    list(APPEND _ffmpeg_libraries "${_ffmpeg_library_dir}/lib${_component}.so")
endforeach()
add_library(FFmpeg::FFmpeg INTERFACE IMPORTED GLOBAL)
set_target_properties(FFmpeg::FFmpeg PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_ffmpeg_include_dir}"
    INTERFACE_LINK_LIBRARIES "${_ffmpeg_libraries}"
    INTERFACE_LINK_OPTIONS "-Wl,-rpath-link,${_ffmpeg_library_dir}")

set(_ffmpeg_staged_lib_dir "${PROJECT_BINARY_DIR}/lib/AudioLibraryToolkit")
file(MAKE_DIRECTORY "${_ffmpeg_staged_lib_dir}")
file(GLOB _ffmpeg_shared_objects "${_ffmpeg_library_dir}/*.so*")
file(COPY ${_ffmpeg_shared_objects} DESTINATION "${_ffmpeg_staged_lib_dir}")
foreach(_library IN LISTS _ffmpeg_shared_objects)
    get_filename_component(_name "${_library}" NAME)
    if(NOT IS_SYMLINK "${_library}")
        execute_process(COMMAND "${PATCHELF_EXECUTABLE}" --set-rpath "$ORIGIN"
            "${_ffmpeg_staged_lib_dir}/${_name}"
            COMMAND_ERROR_IS_FATAL ANY)
    endif()
endforeach()
