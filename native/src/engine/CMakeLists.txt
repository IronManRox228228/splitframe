# Headless engine, local API (HTTP + stdio JSON-RPC), MCP server and the splitframe-cli executable.
# sf_engine has no Qt Quick dependency: the GUI, the CLI and the tests all link it.
add_library(sf_engine STATIC
  result.h
  jschema.h jschema.cpp
  engine.h engine.cpp
  cmd_util.h cmd_util.cpp
  cmd_session.cpp
  cmd_read.cpp
  cmd_edit.cpp
  cmd_media.cpp
  cmd_render.cpp
  cmd_colour.cpp
  cmd_audio.cpp
  cmd_timeline.cpp
  cmd_safety.cpp
  safety_analysis.h safety_analysis.cpp
  timeline_analysis.h timeline_analysis.cpp
  cmd_style.h cmd_style.cpp
  cmd_system.cpp
  cmd_jobs.cpp
  cmd_comfy.h cmd_comfy.cpp
  cmd_variations.cpp
  assembly.h assembly.cpp
  cmd_assemble.cpp
  style_library.h style_library.cpp
  cmd_recipes.cpp
  cmd_titles.cpp
  cmd_export.cpp
  cmd_fx.h cmd_fx.cpp
  hand_quad.h hand_quad.cpp
  one_euro.h
  rpc.h rpc.cpp
  api_server.h api_server.cpp
  stdio.h stdio.cpp
  mcp.h mcp.cpp
  headless.h headless.cpp
)
# The style library (looks, pacing and mix profiles) is JSON compiled into the library: engine/style_library.h.
qt_add_resources(sf_engine "sf_engine_styles"
  PREFIX "/sf/style"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/styles"
  FILES styles/looks.json styles/pacing.json styles/mix.json styles/audio_presets.json styles/safe_zones.json styles/recipes.json styles/titles.json)
target_include_directories(sf_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_engine PUBLIC sf_editor sf_export sf_sys sf_jobs sf_comfy sf_render sf_audio sf_media sf_core sf_index Qt6::Core Qt6::Gui Qt6::Network PRIVATE sf_options)
target_compile_definitions(sf_engine PRIVATE SF_COMFY_TEMPLATES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/comfy_templates")

# Console front end: no window, no QML. FFmpeg / OpenColorIO DLLs land next to it like for the app.
qt_add_executable(splitframe-cli cli/main.cpp)
target_link_libraries(splitframe-cli PRIVATE sf_ai sf_engine sf_options)
add_custom_command(TARGET splitframe-cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:splitframe-cli>"
  COMMAND_EXPAND_LISTS)
