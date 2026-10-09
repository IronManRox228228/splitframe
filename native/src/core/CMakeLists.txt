add_library(sf_core STATIC
  time.h time.cpp
  error.h
  json_reader.h json_reader.cpp
  schema.h schema.cpp
  ops.h ops.cpp
  timeline_doc.h timeline_doc.cpp
  apply.h apply.cpp
  history.h history.cpp
  snap.h snap.cpp
)
target_include_directories(sf_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_core PUBLIC Qt6::Core PRIVATE sf_options)
