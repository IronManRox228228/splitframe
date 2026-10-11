add_library(sf_copilot STATIC
  types.cpp
  lint_engine.cpp
  branch_manager.cpp
  suggestion_engine.cpp
  review_engine.cpp
  copilot_commands.cpp
)

target_include_directories(sf_copilot PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_link_libraries(sf_copilot PUBLIC
  sf_engine
  sf_agent
  sf_index
  sf_editor
  sf_core
  Qt6::Core
  Qt6::Gui
  Qt6::Sql
)

if(MSVC)
  target_compile_options(sf_copilot PRIVATE /W4)
  if(SF_WARNINGS_AS_ERRORS)
    target_compile_options(sf_copilot PRIVATE /WX)
  endif()
endif()
