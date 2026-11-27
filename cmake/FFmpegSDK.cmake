find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
file(READ "${PROJECT_SOURCE_DIR}/cmake/ffmpeg-lock.json" _ffmpeg_lock)
string(JSON AFC_FFMPEG_BUILD GET "${_ffmpeg_lock}" build)
if(WIN32)
    set(_ffmpeg_platform windows-x64)
else()
    set(_ffmpeg_platform linux-x64)
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
        NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
    message(FATAL_ERROR "The locked FFmpeg SDK requires x86_64.")
endif()
if(NOT DEFINED FFmpeg_ROOT)
    if(DEFINED ENV{FFmpeg_ROOT})
        set(FFmpeg_ROOT "$ENV{FFmpeg_ROOT}")
    else()
        set(FFmpeg_ROOT "${PROJECT_SOURCE_DIR}/external/ffmpeg/${AFC_FFMPEG_BUILD}/${_ffmpeg_platform}")
    endif()
endif()
file(TO_CMAKE_PATH "${FFmpeg_ROOT}" FFmpeg_ROOT)
set(FFmpeg_ROOT "${FFmpeg_ROOT}" CACHE PATH "Verified locked FFmpeg SDK" FORCE)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/prepare_ffmpeg.py"
        --verify-only --platform "${_ffmpeg_platform}" --root "${FFmpeg_ROOT}"
    RESULT_VARIABLE _ffmpeg_verified OUTPUT_VARIABLE _ffmpeg_message ERROR_VARIABLE _ffmpeg_error)
if(NOT _ffmpeg_verified STREQUAL "0")
    message(FATAL_ERROR "${_ffmpeg_error}\nRun python tools/prepare_ffmpeg.py before configuring. "
        "Remove a stale FFmpeg_ROOT cache value or select the restored SDK with -DFFmpeg_ROOT:PATH=<path>.")
endif()
set(_ffmpeg_include_dir "${FFmpeg_ROOT}/include")
set(_ffmpeg_library_dir "${FFmpeg_ROOT}/lib")
set(_ffmpeg_binary_dir "${FFmpeg_ROOT}/bin")
set(_ffmpeg_suffix "")
if(WIN32)
    set(_ffmpeg_suffix ".exe")
endif()
# Assign exact paths even when a previous CMake cache selected tools from PATH.
set(FFMPEG_EXECUTABLE "${_ffmpeg_binary_dir}/ffmpeg${_ffmpeg_suffix}" CACHE FILEPATH "Locked FFmpeg tool" FORCE)
set(FFPROBE_EXECUTABLE "${_ffmpeg_binary_dir}/ffprobe${_ffmpeg_suffix}" CACHE FILEPATH "Locked FFprobe tool" FORCE)
configure_file("${PROJECT_SOURCE_DIR}/cmake/FFmpegBuild.h.in"
    "${PROJECT_BINARY_DIR}/generated/FFmpegBuild.h" @ONLY)

function(afc_stage_ffmpeg_runtime target)
    if(WIN32)
        afc_copy_windows_ffmpeg_runtime("${target}")
    else()
        set_target_properties("${target}" PROPERTIES
            BUILD_RPATH "$ORIGIN/../lib/AudioLibraryToolkit"
            INSTALL_RPATH "$ORIGIN/../lib/AudioLibraryToolkit"
            BUILD_WITH_INSTALL_RPATH TRUE)
    endif()
    add_custom_command(TARGET "${target}" POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${FFMPEG_EXECUTABLE}" "${FFPROBE_EXECUTABLE}" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
    if(NOT WIN32)
        add_custom_command(TARGET "${target}" POST_BUILD
            COMMAND "${PATCHELF_EXECUTABLE}" --set-rpath "$ORIGIN/../lib/AudioLibraryToolkit"
                "$<TARGET_FILE_DIR:${target}>/ffmpeg"
            COMMAND "${PATCHELF_EXECUTABLE}" --set-rpath "$ORIGIN/../lib/AudioLibraryToolkit"
                "$<TARGET_FILE_DIR:${target}>/ffprobe"
            VERBATIM)
    endif()
endfunction()
