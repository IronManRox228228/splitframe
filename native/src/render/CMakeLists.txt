add_library(sf_render STATIC
  layout.h layout.cpp
  playback_clock.h playback_clock.cpp
  media_provider.h media_provider.cpp
  layer_painter.h layer_painter.cpp
  gpu_planes.h gpu_planes.cpp
  color.h color.cpp
  color_manager.h color_manager.cpp
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

# OpenColorIO is optional (see the top-level CMakeLists). Its DLL goes next to every binary, like
# FFmpeg's does for the app.
if(OpenColorIO_FOUND)
  target_link_libraries(sf_render PRIVATE OpenColorIO::OpenColorIO)
  target_compile_definitions(sf_render PRIVATE SF_HAVE_OCIO=1)
  add_custom_target(sf_ocio_dlls ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:OpenColorIO::OpenColorIO>" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMENT "Copying the OpenColorIO runtime next to the binaries")
  add_dependencies(sf_render sf_ocio_dlls)
endif()

# Shaders go through qsb into one .qsb each (SPIR-V plus HLSL for D3D11, MSL for Metal) and live in the
# library's resources under :/sf/render/shaders/.
qt_add_shaders(sf_render "sf_render_shaders"
  PREFIX "/sf/render"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}"
  GLSL "330"
  HLSL "50"
  MSL "12"
  FILES shaders/layer.vert shaders/layer.frag shaders/output.frag
)
