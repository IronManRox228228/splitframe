# System profile and resource policy: DXGI / CPU / RAM / power queries, the FFmpeg hardware smoke test, the pure decision
# function and llama.cpp fit advice. Qt Core + FFmpeg only (the renderer is not linked: the caller passes a compositor probe).
add_library(sf_sys STATIC
  system_profile.h system_profile.cpp
)
target_include_directories(sf_sys PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_sys
  PUBLIC Qt6::Core
  PRIVATE sf_options FFmpeg::avcodec FFmpeg::avutil)
if(WIN32)
  target_link_libraries(sf_sys PRIVATE dxgi)
endif()
