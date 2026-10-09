add_library(sf_core STATIC
  time.h time.cpp
)
target_include_directories(sf_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_core PUBLIC Qt6::Core PRIVATE sf_options)
