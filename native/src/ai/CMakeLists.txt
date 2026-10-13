# AI wiring: registers the agent, copilot, branch and index commands into an Engine (GUI, splitframe-cli, MCP).
add_library(sf_ai STATIC
  ai_services.h ai_services.cpp
)
target_include_directories(sf_ai PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_ai PUBLIC sf_copilot sf_index sf_agent sf_engine sf_editor sf_core Qt6::Core PRIVATE sf_options)
