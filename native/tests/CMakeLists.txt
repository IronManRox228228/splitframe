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

# Renderer: compositor (GPU, offscreen QRhi), layout maths, playback clock.
function(sf_add_render_test name)
  qt_add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE sf_render sf_core sf_media sf_options Qt6::Test)
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

sf_add_render_test(tst_playback_clock)
sf_add_render_test(tst_layout)
sf_add_render_test(tst_compositor)

# Editor (project lifecycle, media pool, timeline models/controller, inspector) and the UI on top.
function(sf_add_editor_test name)
  cmake_parse_arguments(T "" "" "SOURCES;LIBS;ENV" ${ARGN})
  qt_add_executable(${name} ${name}.cpp ${T_SOURCES})
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(${name} PRIVATE sf_editor sf_media sf_core sf_options Qt6::Test ${T_LIBS})
  add_test(NAME ${name} COMMAND ${name})
  get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_RELEASE)
  if(NOT _qtcore)
    get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_DEBUG)
  endif()
  get_filename_component(_qtbin "${_qtcore}" DIRECTORY)
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin};PATH=path_list_prepend:${FFMPEG_ROOT}/bin"
    ENVIRONMENT "SF_FFMPEG_EXE=${FFMPEG_EXECUTABLE};${T_ENV}")
endfunction()

sf_add_editor_test(tst_timeline_math)
sf_add_editor_test(tst_project)
sf_add_editor_test(tst_timeline_model)
sf_add_editor_test(tst_timeline_controller)
sf_add_editor_test(tst_inspector)
sf_add_editor_test(tst_media_pool)

# The painted timeline is compiled into its own test (the app's copy lives in the exe).
sf_add_editor_test(tst_timeline_view
  SOURCES "${CMAKE_SOURCE_DIR}/src/app/timeline_view.cpp" "${CMAKE_SOURCE_DIR}/src/app/timeline_view.h"
  LIBS Qt6::Quick
  ENV "QT_QPA_PLATFORM=offscreen")

sf_add_editor_test(tst_ui_smoke)
add_dependencies(tst_ui_smoke splitframe)
target_compile_definitions(tst_ui_smoke PRIVATE SF_APP_EXE="$<TARGET_FILE:splitframe>")
