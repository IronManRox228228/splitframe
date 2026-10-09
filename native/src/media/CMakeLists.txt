add_library(sf_media STATIC
  ffmpeg.h
  probe.h probe.cpp
)
target_include_directories(sf_media PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_media
  PUBLIC sf_core Qt6::Gui
  PRIVATE sf_options FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale)
