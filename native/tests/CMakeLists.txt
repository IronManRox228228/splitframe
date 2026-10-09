# One QtTest executable per area. Tests find Qt and FFmpeg DLLs through PATH set here, so they run
# straight from the build tree without deploying.
function(sf_add_test name)
  qt_add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE sf_core sf_media sf_options Qt6::Test)
  add_test(NAME ${name} COMMAND ${name})
  get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_RELEASE)
  if(NOT _qtcore)
    get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_DEBUG)
  endif()
  get_filename_component(_qtbin "${_qtcore}" DIRECTORY)
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin};PATH=path_list_prepend:${FFMPEG_ROOT}/bin"
    ENVIRONMENT "SF_FFMPEG_EXE=${FFMPEG_EXECUTABLE}")
endfunction()

sf_add_test(tst_time)
sf_add_test(tst_probe)
sf_add_test(tst_video_decoder)
sf_add_test(tst_media_probe)
sf_add_test(tst_audio_decoder)
sf_add_test(tst_thumbnails)
sf_add_test(tst_frame_cache)

# Core (project model) suites: QtCore/QtTest only, no FFmpeg. Fixtures are generated from the TS
# packages by fixtures/gen (see its README comment) and committed.
function(sf_add_core_test name)
  qt_add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE sf_core sf_options Qt6::Test)
  target_compile_definitions(${name} PRIVATE SF_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  add_test(NAME ${name} COMMAND ${name})
  get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_RELEASE)
  if(NOT _qtcore)
    get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_DEBUG)
  endif()
  get_filename_component(_qtbin "${_qtcore}" DIRECTORY)
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin}")
endfunction()

sf_add_core_test(tst_schema)
sf_add_core_test(tst_core)
sf_add_core_test(tst_history_ripple)
sf_add_core_test(tst_invariants)
sf_add_core_test(tst_snap)
sf_add_core_test(tst_trim_split)
sf_add_core_test(tst_golden)
