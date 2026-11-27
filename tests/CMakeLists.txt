find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(NAME ffmpeg-sdk-setup
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg_setup.py")
add_test(NAME ffmpeg-bundled-runtime
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/bundled_runtime.py"
        --converter "$<TARGET_FILE:AudioLibraryToolkit>")
add_test(NAME taglib-update-check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/taglib_update_check.py"
        "${CMAKE_COMMAND}" "${PROJECT_SOURCE_DIR}/cmake/CheckTagLibUpdate.cmake")
add_test(NAME dependency-version-report
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/dependency_versions.py"
        "${CMAKE_COMMAND}" "${PROJECT_SOURCE_DIR}/cmake/PrintDependencyVersions.cmake")

add_executable(preservation_audit_test preservation_audit.cpp)
target_link_libraries(preservation_audit_test PRIVATE AudioLibraryToolkitCore)
add_executable(warning_regressions_test warning_regressions.cpp)
target_link_libraries(warning_regressions_test PRIVATE AudioLibraryToolkitCore)
add_executable(media_metadata_json_test media_metadata_json.cpp)
target_link_libraries(media_metadata_json_test PRIVATE AudioLibraryToolkitCore)
add_executable(media_record_json_test media_record_json.cpp)
target_link_libraries(media_record_json_test PRIVATE AudioLibraryToolkitCore)
add_executable(taglib_smoke_test taglib_smoke.cpp)
target_link_libraries(taglib_smoke_test PRIVATE AudioLibraryToolkitCore)
add_executable(decode_analyzer_test decode_analyzer.cpp)
target_link_libraries(decode_analyzer_test PRIVATE AudioLibraryToolkitCore)
add_executable(bounded_task_runner_test bounded_task_runner.cpp)
target_include_directories(bounded_task_runner_test PRIVATE
    "${PROJECT_SOURCE_DIR}/AudioLibraryToolkit/include/common")
target_compile_features(bounded_task_runner_test PRIVATE cxx_std_20)
add_executable(scoped_current_directory_test scoped_current_directory.cpp)
target_link_libraries(scoped_current_directory_test PRIVATE AudioLibraryToolkitCore)
add_executable(scan_report_schema_test scan_report_schema.cpp)
target_link_libraries(scan_report_schema_test PRIVATE AudioLibraryToolkitCore)
add_executable(duplicate_review_test duplicate_review.cpp)
target_link_libraries(duplicate_review_test PRIVATE AudioLibraryToolkitCore)
add_executable(duplicate_matcher_test duplicate_matcher.cpp)
target_link_libraries(duplicate_matcher_test PRIVATE AudioLibraryToolkitCore)
add_executable(duplicate_comparison_test duplicate_comparison.cpp)
target_link_libraries(duplicate_comparison_test PRIVATE AudioLibraryToolkitCore)
add_executable(run_logging_test run_logging.cpp)
target_link_libraries(run_logging_test PRIVATE AudioLibraryToolkitCore)
add_executable(app_settings_config_test app_settings_config.cpp)
target_link_libraries(app_settings_config_test PRIVATE AudioLibraryToolkitCore)

foreach(_target IN ITEMS preservation_audit_test warning_regressions_test media_metadata_json_test media_record_json_test taglib_smoke_test decode_analyzer_test bounded_task_runner_test scoped_current_directory_test scan_report_schema_test duplicate_review_test duplicate_matcher_test duplicate_comparison_test run_logging_test app_settings_config_test)
    target_compile_options(${_target} PRIVATE
        "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/W3;/utf-8;/permissive->"
        "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>>:-Wall;-Wextra;-Wpedantic>")
    afc_stage_ffmpeg_runtime(${_target})
endforeach()

add_test(NAME preservation-audit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_native_regression.py"
        preservation
        "$<TARGET_FILE:preservation_audit_test>"
        "${FFMPEG_EXECUTABLE}")
add_test(NAME warning-regressions
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_native_regression.py"
        warnings
        "$<TARGET_FILE:warning_regressions_test>"
        "${FFMPEG_EXECUTABLE}")
add_test(NAME media-metadata-json
    COMMAND "$<TARGET_FILE:media_metadata_json_test>")
add_test(NAME media-record-json
    COMMAND "$<TARGET_FILE:media_record_json_test>"
        "${PROJECT_SOURCE_DIR}/docs/schemas/media-catalog-v2.schema.json")
add_test(NAME scan-root-level-audio
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scan_root_audio.py"
        "$<TARGET_FILE:AudioLibraryToolkit>"
        "${FFMPEG_EXECUTABLE}"
        "$<TARGET_FILE:scan_report_schema_test>"
        "${PROJECT_SOURCE_DIR}/docs/schemas/scan-report-v2.schema.json"
        "${PROJECT_SOURCE_DIR}/docs/schemas/scan-manifest-v1.schema.json")
add_test(NAME taglib-reader-smoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_native_regression.py"
        taglib
        "$<TARGET_FILE:taglib_smoke_test>"
        "${FFMPEG_EXECUTABLE}")
add_test(NAME decode-analyzer
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/decode_analyzer.py"
        --analyzer "$<TARGET_FILE:decode_analyzer_test>"
        --ffmpeg "${FFMPEG_EXECUTABLE}")
add_test(NAME bounded-task-runner
    COMMAND "$<TARGET_FILE:bounded_task_runner_test>")
add_test(NAME scoped-current-directory
    COMMAND "$<TARGET_FILE:scoped_current_directory_test>")
add_test(NAME duplicate-review
    COMMAND "$<TARGET_FILE:duplicate_review_test>")
add_test(NAME duplicate-matcher
    COMMAND "$<TARGET_FILE:duplicate_matcher_test>")
add_test(NAME duplicate-comparison
    COMMAND "$<TARGET_FILE:duplicate_comparison_test>")
add_test(NAME run-logging
    COMMAND "$<TARGET_FILE:run_logging_test>")
add_test(NAME app-settings-config
    COMMAND "$<TARGET_FILE:app_settings_config_test>"
        "${PROJECT_SOURCE_DIR}/AudioLibraryToolkit/config.default.json")
add_test(NAME config-startup
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/config_startup.py"
        "$<TARGET_FILE:AudioLibraryToolkit>")
add_test(NAME worker-protocol
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/worker_protocol.py"
        "$<TARGET_FILE:AudioLibraryToolkit>")
if(WIN32)
    add_test(NAME config-windows-console
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/windows_console_setup.py"
            "$<TARGET_FILE:AudioLibraryToolkit>")
    add_test(NAME progress-windows-console
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/windows_console_setup.py"
            "$<TARGET_FILE:run_logging_test>" --progress)
endif()
add_test(NAME mp3-conversion-integration
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/audio_library_integration.py"
        --converter "$<TARGET_FILE:AudioLibraryToolkit>"
        --ffmpeg "${FFMPEG_EXECUTABLE}"
        --ffprobe "${FFPROBE_EXECUTABLE}"
        --audit-checker "$<TARGET_FILE:preservation_audit_test>"
        --taglib-debug "$<IF:$<OR:$<CONFIG:Debug>,$<BOOL:${TRACE_IN_RELEASE}>>,1,0>")
set_tests_properties(mp3-conversion-integration PROPERTIES TIMEOUT 900)
