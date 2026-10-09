add_library(sf_media STATIC
  ffmpeg.h ffmpeg.cpp
  packet_index.h packet_index.cpp
  frame_convert.h frame_convert.cpp
  hwdevice.h hwdevice.cpp
  probe.h probe.cpp
  video_decoder.h video_decoder.cpp
  frame_cache.h frame_cache.cpp
  frame_service.h frame_service.cpp
  audio_decoder.h audio_decoder.cpp
  waveform.h waveform.cpp
  thumbnails.h thumbnails.cpp
)
target_include_directories(sf_media PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_media
  PUBLIC sf_core Qt6::Gui
  PRIVATE sf_options FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample)
