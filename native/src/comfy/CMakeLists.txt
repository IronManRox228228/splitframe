# ComfyUI core: schema parsing, typed graph, validation and HTTP client.
add_library(sf_comfy STATIC
  comfy_schema.h comfy_schema.cpp
  comfy_graph.h comfy_graph.cpp
  comfy_validate.h comfy_validate.cpp
  comfy_client.h comfy_client.cpp
  comfy_generate.h comfy_generate.cpp
  comfy_process.h comfy_process.cpp
)
target_include_directories(sf_comfy PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_comfy PUBLIC Qt6::Core Qt6::Network sf_jobs PRIVATE sf_options)
