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
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin};PATH=path_list_prepend:${FFMPEG_ROOT}/bin;PATH=path_list_prepend:${OCIO_ROOT}/bin"
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
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin};PATH=path_list_prepend:${FFMPEG_ROOT}/bin;PATH=path_list_prepend:${OCIO_ROOT}/bin"
    ENVIRONMENT "SF_FFMPEG_EXE=${FFMPEG_EXECUTABLE}")
endfunction()

sf_add_render_test(tst_playback_clock)
sf_add_render_test(tst_layout)
sf_add_render_test(tst_compositor)
sf_add_render_test(tst_color)

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
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin};PATH=path_list_prepend:${FFMPEG_ROOT}/bin;PATH=path_list_prepend:${OCIO_ROOT}/bin"
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

# Audio engine: renderer, DSP, loudness, mixer schema, CLAP host (with a tiny test plugin), output device.
function(sf_add_audio_test name)
  cmake_parse_arguments(T "" "" "SOURCES;DEFS" ${ARGN})
  qt_add_executable(${name} ${name}.cpp ${T_SOURCES})
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(${name} PRIVATE sf_audio sf_media sf_core sf_options Qt6::Test)
  if(T_DEFS)
    target_compile_definitions(${name} PRIVATE ${T_DEFS})
  endif()
  add_test(NAME ${name} COMMAND ${name})
  get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_RELEASE)
  if(NOT _qtcore)
    get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_DEBUG)
  endif()
  get_filename_component(_qtbin "${_qtcore}" DIRECTORY)
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin};PATH=path_list_prepend:${FFMPEG_ROOT}/bin;PATH=path_list_prepend:$ENV{OCIO_ROOT}/bin"
    ENVIRONMENT "SF_FFMPEG_EXE=${FFMPEG_EXECUTABLE}")
endfunction()

add_library(sf_test_clap_gain MODULE clap_test_gain.cpp)
target_include_directories(sf_test_clap_gain SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party/clap/include")
set_target_properties(sf_test_clap_gain PROPERTIES PREFIX "" SUFFIX ".clap"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/clap" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/clap")

sf_add_audio_test(tst_audio_render)
sf_add_audio_test(tst_audio_dsp)
sf_add_audio_test(tst_loudness)
sf_add_audio_test(tst_mixer_schema)
sf_add_audio_test(tst_clap_host DEFS "SF_TEST_CLAP=\"$<TARGET_FILE:sf_test_clap_gain>\"")
add_dependencies(tst_clap_host sf_test_clap_gain)
sf_add_audio_test(tst_audio_output SOURCES "${CMAKE_SOURCE_DIR}/src/render/playback_clock.cpp")

# Export (offscreen render + encode + mux, headless CLI) and proxies. Tiny projects only; the compositor needs D3D11
# (the suites skip without it). NVENC tests are opt-in (SF_TEST_NVENC=1) and never run by default.
function(sf_add_export_test name)
  cmake_parse_arguments(T "" "" "SOURCES;DEFS" ${ARGN})
  qt_add_executable(${name} ${name}.cpp ${T_SOURCES})
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(${name} PRIVATE sf_export sf_editor sf_render sf_audio sf_media sf_core sf_options Qt6::Test)
  if(T_DEFS)
    target_compile_definitions(${name} PRIVATE ${T_DEFS})
  endif()
  add_test(NAME ${name} COMMAND ${name})
  get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_RELEASE)
  if(NOT _qtcore)
    get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_DEBUG)
  endif()
  get_filename_component(_qtbin "${_qtcore}" DIRECTORY)
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin};PATH=path_list_prepend:${FFMPEG_ROOT}/bin;PATH=path_list_prepend:$ENV{OCIO_ROOT}/bin"
    ENVIRONMENT "SF_FFMPEG_EXE=${FFMPEG_EXECUTABLE}")
endfunction()

sf_add_export_test(tst_export)
sf_add_export_test(tst_proxy)
sf_add_export_test(tst_export_cli DEFS "SF_APP_EXE=\"$<TARGET_FILE:splitframe>\"")
add_dependencies(tst_export_cli splitframe)

# Headless engine, local HTTP API, MCP server and splitframe-cli. Loopback servers and child processes only; frame renders are tiny
# (<= 320x180, software decode) and NVENC is never used.
function(sf_add_engine_test name)
  cmake_parse_arguments(T "" "" "SOURCES;DEFS" ${ARGN})
  qt_add_executable(${name} ${name}.cpp ${T_SOURCES})
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(${name} PRIVATE sf_engine sf_options Qt6::Test Qt6::Network)
  if(T_DEFS)
    target_compile_definitions(${name} PRIVATE ${T_DEFS})
  endif()
  add_test(NAME ${name} COMMAND ${name})
  get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_RELEASE)
  if(NOT _qtcore)
    get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_DEBUG)
  endif()
  get_filename_component(_qtbin "${_qtcore}" DIRECTORY)
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin};PATH=path_list_prepend:${FFMPEG_ROOT}/bin;PATH=path_list_prepend:$ENV{OCIO_ROOT}/bin"
    ENVIRONMENT "SF_FFMPEG_EXE=${FFMPEG_EXECUTABLE}")
endfunction()

sf_add_engine_test(tst_engine)
sf_add_engine_test(tst_api_http)
sf_add_engine_test(tst_mcp DEFS "SF_APP_EXE=\"$<TARGET_FILE:splitframe>\"" "SF_CLI_EXE=\"$<TARGET_FILE:splitframe-cli>\"")
add_dependencies(tst_mcp splitframe splitframe-cli)
sf_add_engine_test(tst_cli DEFS "SF_CLI_EXE=\"$<TARGET_FILE:splitframe-cli>\"")
add_dependencies(tst_cli splitframe-cli)

# Agent harness: router, peer bus, specialists, and multi-agent coordinator.
function(sf_add_agent_test name)
  cmake_parse_arguments(T "" "" "SOURCES;DEFS" ${ARGN})
  qt_add_executable(${name} ${name}.cpp ${T_SOURCES})
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(${name} PRIVATE sf_agent sf_engine sf_options Qt6::Test Qt6::Network)
  if(T_DEFS)
    target_compile_definitions(${name} PRIVATE ${T_DEFS})
  endif()
  add_test(NAME ${name} COMMAND ${name})
  get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_RELEASE)
  if(NOT _qtcore)
    get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_DEBUG)
  endif()
  get_filename_component(_qtbin "${_qtcore}" DIRECTORY)
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin};PATH=path_list_prepend:${FFMPEG_ROOT}/bin;PATH=path_list_prepend:$ENV{OCIO_ROOT}/bin"
    ENVIRONMENT "SF_FFMPEG_EXE=${FFMPEG_EXECUTABLE}")
endfunction()

sf_add_agent_test(tst_agent_router)
sf_add_agent_test(tst_agent_peer_bus)
sf_add_agent_test(tst_agent_specialists)
sf_add_agent_test(tst_agent_harness)

# Footage understanding and indexing tests (M6)
function(sf_add_index_test name)
  cmake_parse_arguments(T "" "" "SOURCES;LIBS;DEFS" ${ARGN})
  qt_add_executable(${name} ${name}.cpp ${T_SOURCES})
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(${name} PRIVATE sf_index sf_agent sf_engine sf_audio sf_media sf_core sf_options Qt6::Test Qt6::Sql Qt6::Gui ${T_LIBS})
  if(WIN32)
    target_link_libraries(${name} PRIVATE windowsapp.lib)
  endif()
  if(T_DEFS)
    target_compile_definitions(${name} PRIVATE ${T_DEFS})
  endif()
  add_test(NAME ${name} COMMAND ${name})
  get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_RELEASE)
  if(NOT _qtcore)
    get_target_property(_qtcore Qt6::Core IMPORTED_LOCATION_DEBUG)
  endif()
  get_filename_component(_qtbin "${_qtcore}" DIRECTORY)
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_qtbin};PATH=path_list_prepend:${FFMPEG_ROOT}/bin;PATH=path_list_prepend:$ENV{OCIO_ROOT}/bin"
    ENVIRONMENT "SF_FFMPEG_EXE=${FFMPEG_EXECUTABLE}")
endfunction()

sf_add_index_test(tst_index_catalog)
sf_add_index_test(tst_index_video)
sf_add_index_test(tst_index_audio)
sf_add_index_test(tst_index_ocr)
sf_add_index_test(tst_index_commands)
sf_add_index_test(tst_index_manager)

