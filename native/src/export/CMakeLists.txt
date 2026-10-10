# Export (timeline -> file through the offscreen compositor and the audio engine) and proxy generation.
# Needs the GPL FFmpeg build: libx264 / libx265 (see ../../README.md).
add_library(sf_export STATIC
  export_settings.h export_settings.cpp
  exporter.h exporter.cpp
  proxy.h proxy.cpp
  export_cli.h export_cli.cpp
)
target_include_directories(sf_export PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_export
  PUBLIC sf_core sf_media sf_render sf_audio Qt6::Core Qt6::Gui
  PRIVATE sf_options FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale)
