if(NOT DEFINED TAGLIB_RELEASE_URL)
    set(TAGLIB_RELEASE_URL "https://api.github.com/repos/taglib/taglib/releases/latest")
endif()

set(_taglib_header "${TAGLIB_SOURCE_DIR}/taglib/toolkit/taglib.h")
if(NOT EXISTS "${_taglib_header}")
    message(STATUS "TagLib update check unavailable: version header not found; build continues.")
    return()
endif()

foreach(_part IN ITEMS MAJOR MINOR PATCH)
    file(STRINGS "${_taglib_header}" _version_line
        REGEX "^#define[ \t]+TAGLIB_${_part}_VERSION[ \t]+[0-9]+")
    if(NOT _version_line MATCHES "^#define[ \t]+TAGLIB_${_part}_VERSION[ \t]+([0-9]+)")
        message(STATUS "TagLib update check unavailable: cannot determine current version; build continues.")
        return()
    endif()
    set(_version_${_part} "${CMAKE_MATCH_1}")
endforeach()
set(_current "${_version_MAJOR}.${_version_MINOR}.${_version_PATCH}")

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef _suffix)
set(_response_file "${TAGLIB_UPDATE_TMP_DIR}/taglib-release-${_suffix}.json")
file(DOWNLOAD "${TAGLIB_RELEASE_URL}" "${_response_file}"
    STATUS _download_status
    TIMEOUT 5
    TLS_VERIFY ON
    HTTPHEADER "Accept: application/vnd.github+json")
list(GET _download_status 0 _status_code)
if(NOT _status_code EQUAL 0)
    file(REMOVE "${_response_file}")
    message(STATUS "TagLib update check unavailable (network or GitHub error); using ${_current}. Build continues.")
    return()
endif()

file(READ "${_response_file}" _response)
file(REMOVE "${_response_file}")
string(JSON _tag ERROR_VARIABLE _json_error GET "${_response}" tag_name)
if(_json_error OR NOT _tag MATCHES "^v([0-9]+\\.[0-9]+(\\.[0-9]+)?)$")
    message(STATUS "TagLib update check unavailable: unrecognized release response; using ${_current}. Build continues.")
    return()
endif()
set(_latest "${CMAKE_MATCH_1}")
if(_latest VERSION_GREATER _current)
    message(STATUS
        "TagLib update available: using ${_current}; latest stable is ${_latest}. "
        "See https://github.com/taglib/taglib/releases/tag/${_tag}. The pinned version is unchanged.")
    message(STATUS "TagLib update check succeeded; this notice does not stop the build.")
    message(STATUS "For the overall build result, check the build command's exit code (0 = success).")
else()
    message(STATUS "TagLib update check: using ${_current}; latest stable is ${_latest}.")
endif()
