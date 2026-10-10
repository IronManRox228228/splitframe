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
  cmd_export.cpp
  rpc.h rpc.cpp
  api_server.h api_server.cpp
  stdio.h stdio.cpp
  mcp.h mcp.cpp
  headless.h headless.cpp
)
target_include_directories(sf_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_engine PUBLIC sf_editor sf_export sf_render sf_audio sf_media sf_core Qt6::Core Qt6::Gui Qt6::Network PRIVATE sf_options)

# Console front end: no window, no QML. FFmpeg / OpenColorIO DLLs land next to it like for the app.
qt_add_executable(splitframe-cli cli/main.cpp)
target_link_libraries(splitframe-cli PRIVATE sf_engine sf_options)
add_custom_command(TARGET splitframe-cli POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:splitframe-cli>"
  COMMAND_EXPAND_LISTS)
