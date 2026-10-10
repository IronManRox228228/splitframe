# Agent harness: three specialists (editor, colourist, audio), peer bus, router, and modes.
add_library(sf_agent STATIC
  types.h
  peer_bus.h peer_bus.cpp
  router.h router.cpp
  llm_client.h llm_client.cpp
  specialist.h specialist.cpp
  agent_harness.h agent_harness.cpp
)
target_include_directories(sf_agent PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_agent PUBLIC sf_engine sf_editor sf_core Qt6::Core Qt6::Network PRIVATE sf_options)
