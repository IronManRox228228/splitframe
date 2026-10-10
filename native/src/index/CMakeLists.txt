# Footage understanding and indexing: catalog, video/audio analysis, WinRT OCR, VLM tags, search.
add_library(sf_index STATIC
  types.h types.cpp
  catalog.h catalog.cpp
  video_analyzer.h video_analyzer.cpp
  audio_analyzer.h audio_analyzer.cpp
  ocr_bridge.h ocr_bridge.cpp
  vlm_descriptor.h vlm_descriptor.cpp
  index_manager.h index_manager.cpp
  index_commands.h index_commands.cpp
)

target_include_directories(sf_index PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_index
  PUBLIC
    sf_core
    sf_media
    sf_audio
    sf_agent
    sf_engine
    Qt6::Core
    Qt6::Sql
    Qt6::Gui
    FFmpeg::avfilter
    FFmpeg::avformat
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swscale
  PRIVATE
    sf_options
)

if(WIN32)
  target_link_libraries(sf_index PUBLIC windowsapp.lib)
endif()
