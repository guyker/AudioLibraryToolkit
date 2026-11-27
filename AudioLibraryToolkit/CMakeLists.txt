add_library(sqlite3 STATIC
    "${PROJECT_SOURCE_DIR}/external/sqlite-amalgamation/sqlite3.c")
add_library(SQLite::SQLite3 ALIAS sqlite3)
target_include_directories(sqlite3 PUBLIC
    "${PROJECT_SOURCE_DIR}/external/sqlite-amalgamation")
target_link_libraries(sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(sqlite3 PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

add_library(CLI11 INTERFACE)
target_include_directories(CLI11 SYSTEM INTERFACE
    "${PROJECT_SOURCE_DIR}/external/CLI11/include")

add_library(spdlog_headers INTERFACE)
target_include_directories(spdlog_headers SYSTEM INTERFACE
    "${PROJECT_SOURCE_DIR}/external/spdlog/include")

add_library(rapidjson_headers INTERFACE)
target_include_directories(rapidjson_headers SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(AudioLibraryToolkitCore STATIC
    src/catalog/AlbumCollection.cpp
    src/catalog/CatalogDatabase.cpp
    src/catalog/MediaRecordJson.cpp
    src/common/CommonUtils.cpp
    src/common/JsonUtils.cpp
    src/conversion/FolderConvert.cpp
    src/conversion/MediaConversionAsyncTask.cpp
    src/conversion/MediaConversionTask.cpp
    src/duplicates/DuplicateComparison.cpp
    src/duplicates/DuplicateMatcher.cpp
    src/duplicates/DuplicateReview.cpp
    src/media/DecodeAnalyzer.cpp
    src/media/MediaAlbum.cpp
    src/media/MediaTrack.cpp
    src/metadata/FlacMetadata.cpp
    src/metadata/Mp3TagIssues.cpp
    src/metadata/Mp3TagMetadata.cpp
    src/metadata/PreservationAudit.cpp
    src/metadata/TagLibReader.cpp
    src/platform/FFmpeg.cpp
    src/platform/FFmpegRuntime.cpp
    src/platform/PlatformUtils.cpp
    src/repair/AudioRepair.cpp
    src/repair/RepairRules.cpp
    src/app/AppSettings.cpp
    src/app/CommandLine.cpp
    src/app/RunLogging.cpp)

target_include_directories(AudioLibraryToolkitCore PUBLIC
    "${PROJECT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/app"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/catalog"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/conversion"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/duplicates"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/media"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/metadata"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/platform"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/repair")
target_link_libraries(AudioLibraryToolkitCore
    PUBLIC
        CLI11
        FFmpeg::FFmpeg
        SQLite::SQLite3
        spdlog_headers
        TagLib::TagLib
        Threads::Threads
        ${CMAKE_DL_LIBS}
    PRIVATE
        rapidjson_headers)

add_executable(AudioLibraryToolkit
    src/app/AudioLibraryToolkitMain.cpp
    src/app/ApplicationServices.cpp
    src/app/WorkerProtocol.cpp)
target_link_libraries(AudioLibraryToolkit PRIVATE AudioLibraryToolkitCore)
# Stage the first-run configuration template beside the built executable.
add_custom_command(TARGET AudioLibraryToolkit POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.default.json"
        "$<TARGET_FILE_DIR:AudioLibraryToolkit>/config.default.json"
    VERBATIM)

foreach(_target IN ITEMS sqlite3 AudioLibraryToolkitCore AudioLibraryToolkit)
    set_target_properties(${_target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_compile_options(${_target} PRIVATE
        "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/W3;/utf-8;/permissive->"
        "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>>:-Wall;-Wextra;-Wpedantic>")
endforeach()

if(WIN32)
    target_compile_definitions(AudioLibraryToolkitCore PUBLIC UNICODE _UNICODE SPDLOG_WCHAR_FILENAMES)
    target_link_libraries(AudioLibraryToolkitCore PUBLIC shell32)
    afc_embed_windows_manifest(AudioLibraryToolkit)
endif()
afc_stage_ffmpeg_runtime(AudioLibraryToolkit)
