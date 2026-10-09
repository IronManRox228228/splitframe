add_library(sf_render STATIC
  layout.h layout.cpp
  playback_clock.h playback_clock.cpp
  media_provider.h media_provider.cpp
  layer_painter.h layer_painter.cpp
  gpu_planes.h gpu_planes.cpp
  compositor.h compositor.cpp
  offscreen.h offscreen.cpp
)
target_include_directories(sf_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_render
  PUBLIC sf_core sf_media Qt6::Gui Qt6::GuiPrivate
  PRIVATE sf_options)
if(WIN32)
  target_link_libraries(sf_render PRIVATE d3d11 dxgi)
endif()

# Shaders go through qsb into one .qsb each (SPIR-V plus HLSL for D3D11, MSL for Metal) and live in the
# library's resources under :/sf/render/shaders/.
qt_add_shaders(sf_render "sf_render_shaders"
  PREFIX "/sf/render"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}"
  GLSL "330"
  HLSL "50"
  MSL "12"
  FILES shaders/layer.vert shaders/layer.frag
)
