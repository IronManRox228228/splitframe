# Editing logic with no Qt Quick dependency: project lifecycle, media pool, timeline models,
# the timeline controller and the inspector. The app wires these to QML; tests drive them directly.
add_library(sf_editor STATIC
  timeline_math.h timeline_math.cpp
  project.h project.cpp
  media_pool.h media_pool.cpp
  timeline_model.h timeline_model.cpp
  timeline_controller.h timeline_controller.cpp
  inspector.h inspector.cpp
  recent_projects.h recent_projects.cpp
)
target_include_directories(sf_editor PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_editor PUBLIC sf_core sf_media Qt6::Gui PRIVATE sf_options)
