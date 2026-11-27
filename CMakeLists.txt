cmake_minimum_required(VERSION 3.28)

project(AudioLibraryToolkit VERSION 1.0 LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)
include(FetchContent)
cmake_policy(SET CMP0077 NEW)

foreach(_dependency_file IN ITEMS
        "${CMAKE_CURRENT_SOURCE_DIR}/external/CLI11/include/CLI/CLI.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/external/spdlog/include/spdlog/spdlog.h")
    if(NOT EXISTS "${_dependency_file}")
        message(FATAL_ERROR
            "Required Git submodule content is missing: '${_dependency_file}'. "
            "Run 'git submodule update --init --recursive'.")
    endif()
endforeach()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/sqlite-amalgamation/sqlite3.c")
    message(FATAL_ERROR "SQLite amalgamation missing under external/sqlite-amalgamation.")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/AudioLibraryToolkit/include/rapidjson/document.h")
    message(FATAL_ERROR "Vendored RapidJSON headers missing under AudioLibraryToolkit/include/rapidjson.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Load compiler checks before dependencies can capture legacy try_compile policies.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
    "#include <format>
     int main() { return std::format(\"{}\", 1) == \"1\" ? 0 : 1; }"
    AFC_HAS_STD_FORMAT)
if(NOT AFC_HAS_STD_FORMAT)
    message(FATAL_ERROR "The selected C++ standard library must provide C++20 std::format.")
endif()

set(_afc_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_BINDINGS OFF)
set(BUILD_EXAMPLES OFF)
FetchContent_Declare(taglib
    GIT_REPOSITORY https://github.com/taglib/taglib.git
    GIT_TAG deadc2990767dfbda0701e0ab35fdeea653db08f
    GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(taglib)
set(BUILD_TESTING "${_afc_build_testing}")
add_library(TagLib::TagLib ALIAS tag)
target_include_directories(tag INTERFACE
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/toolkit>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/mpeg>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/mpeg/id3v2>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/mpeg/id3v2/frames>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/mpeg/id3v1>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/ogg>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/ogg/flac>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/ogg/vorbis>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/ogg/speex>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/ogg/opus>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/flac>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/ape>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/mpc>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/wavpack>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/mp4>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/riff>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/riff/aiff>"
    "$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/riff/wav>"
    "$<BUILD_INTERFACE:${taglib_BINARY_DIR}>")

find_package(Threads REQUIRED)

include(cmake/FFmpegSDK.cmake)
if(WIN32)
    include(cmake/platform/Windows.cmake)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(cmake/platform/Linux.cmake)
else()
    message(FATAL_ERROR "Unsupported target platform: ${CMAKE_SYSTEM_NAME}")
endif()

add_subdirectory(AudioLibraryToolkit)

find_package(Git QUIET)
set(_afc_ffmpeg_include_dir "${_ffmpeg_include_dir}")
get_directory_property(_afc_utfcpp_found DIRECTORY "${taglib_SOURCE_DIR}" DEFINITION utf8cpp_FOUND)
get_directory_property(_afc_utfcpp_version DIRECTORY "${taglib_SOURCE_DIR}" DEFINITION utf8cpp_VERSION)

add_custom_target(check_taglib_updates ALL
    COMMAND "${CMAKE_COMMAND}"
        "-DREPORT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DREPORT_TAGLIB_SOURCE_DIR=${taglib_SOURCE_DIR}"
        "-DREPORT_TAGLIB_BINARY_DIR=${taglib_BINARY_DIR}"
        "-DREPORT_FFMPEG_INCLUDE_DIR=${_afc_ffmpeg_include_dir}"
        "-DREPORT_FFMPEG_EXECUTABLE=${FFMPEG_EXECUTABLE}"
        "-DREPORT_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DREPORT_UTFCPP_FOUND=${_afc_utfcpp_found}"
        "-DREPORT_UTFCPP_VERSION=${_afc_utfcpp_version}"
        "-DREPORT_ZLIB_INCLUDE_DIR=${ZLIB_INCLUDE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrintDependencyVersions.cmake"
    COMMAND "${CMAKE_COMMAND}"
        "-DTAGLIB_SOURCE_DIR=${taglib_SOURCE_DIR}"
        "-DTAGLIB_UPDATE_TMP_DIR=${CMAKE_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckTagLibUpdate.cmake"
    VERBATIM)
add_dependencies(AudioLibraryToolkitCore check_taglib_updates)

# Keep runtime packaging separate from the fetched TagLib development SDK's install rules.
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME AudioLibraryToolkit)
install(TARGETS AudioLibraryToolkit
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
# The executable finds this template at <bindir>/../share/AudioLibraryToolkit with the default layout.
install(FILES AudioLibraryToolkit/config.default.json
    DESTINATION "${CMAKE_INSTALL_DATADIR}/AudioLibraryToolkit")

install(FILES LICENSE THIRD_PARTY_NOTICES.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES external/CLI11/LICENSE
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses" RENAME CLI11.txt)
install(FILES external/spdlog/LICENSE
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses" RENAME spdlog.txt)
install(FILES external/spdlog/include/spdlog/fmt/bundled/fmt.license.rst
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses" RENAME fmt.txt)
install(FILES licenses/RapidJSON.txt
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses")
install(FILES "${taglib_SOURCE_DIR}/COPYING.LGPL"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses" RENAME TagLib-LGPL.txt)
install(FILES "${taglib_SOURCE_DIR}/COPYING.MPL"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses" RENAME TagLib-MPL.txt)
if(EXISTS "${taglib_SOURCE_DIR}/3rdparty/utfcpp/LICENSE")
    install(FILES "${taglib_SOURCE_DIR}/3rdparty/utfcpp/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses" RENAME utfcpp.txt)
endif()

if(WIN32)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    include(InstallRequiredSystemLibraries)
    install(FILES ${_ffmpeg_runtime_dlls}
        DESTINATION "${CMAKE_INSTALL_BINDIR}")
    if(EXISTS "${FFmpeg_ROOT}/LICENSE.txt")
        install(FILES "${FFmpeg_ROOT}/LICENSE.txt"
            DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses" RENAME FFmpeg.txt)
    else()
        message(WARNING "The selected FFmpeg SDK has no LICENSE.txt; obtain its licence and redistribution notices separately.")
    endif()
endif()

install(PROGRAMS
    "$<TARGET_FILE_DIR:AudioLibraryToolkit>/ffmpeg${_ffmpeg_suffix}"
    "$<TARGET_FILE_DIR:AudioLibraryToolkit>/ffprobe${_ffmpeg_suffix}"
    DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(NOT WIN32)
    install(DIRECTORY "${PROJECT_BINARY_DIR}/lib/AudioLibraryToolkit/"
        DESTINATION "lib/AudioLibraryToolkit" USE_SOURCE_PERMISSIONS)
    install(FILES "${FFmpeg_ROOT}/LICENSE.txt"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses" RENAME FFmpeg.txt)
    configure_file(cmake/InstallLinuxRuntime.cmake.in
        "${PROJECT_BINARY_DIR}/InstallLinuxRuntime.cmake" @ONLY)
    install(SCRIPT "${PROJECT_BINARY_DIR}/InstallLinuxRuntime.cmake")
endif()
install(FILES "${PROJECT_SOURCE_DIR}/cmake/ffmpeg-lock.json"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/AudioLibraryToolkit")

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
