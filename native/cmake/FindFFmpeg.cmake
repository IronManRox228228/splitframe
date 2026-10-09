# Finds a prebuilt shared FFmpeg (headers + import libs + DLLs), e.g. a BtbN "lgpl-shared" build.
# Set FFMPEG_ROOT (cache var or env) to its folder. Defines FFmpeg::<component> imported targets
# and FFMPEG_RUNTIME_DLLS for copying next to executables.

set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}" CACHE PATH "Root of a shared FFmpeg build")
set(_ffmpeg_components avcodec avformat avutil swscale swresample avfilter)

find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h HINTS "${FFMPEG_ROOT}/include")
set(FFMPEG_RUNTIME_DLLS "")
set(_ffmpeg_missing "")

foreach(_c IN LISTS _ffmpeg_components)
  find_library(FFMPEG_${_c}_LIBRARY NAMES ${_c} HINTS "${FFMPEG_ROOT}/lib")
  if(NOT FFMPEG_${_c}_LIBRARY)
    list(APPEND _ffmpeg_missing ${_c})
    continue()
  endif()
  file(GLOB _dll "${FFMPEG_ROOT}/bin/${_c}-*.dll")
  list(APPEND FFMPEG_RUNTIME_DLLS ${_dll})
  if(NOT TARGET FFmpeg::${_c})
    add_library(FFmpeg::${_c} UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::${_c} PROPERTIES
      IMPORTED_LOCATION "${FFMPEG_${_c}_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}")
  endif()
endforeach()

find_program(FFMPEG_EXECUTABLE ffmpeg HINTS "${FFMPEG_ROOT}/bin" NO_DEFAULT_PATH)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
  REQUIRED_VARS FFMPEG_INCLUDE_DIR FFMPEG_avcodec_LIBRARY FFMPEG_avformat_LIBRARY FFMPEG_avutil_LIBRARY
  REASON_FAILURE_MESSAGE "Set FFMPEG_ROOT to a shared FFmpeg build. Missing: ${_ffmpeg_missing}")
