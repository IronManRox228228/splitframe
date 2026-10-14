# Background analysis jobs: the scheduler (queue, disk cache, priorities, pause / cancel), ONNX Runtime wrapper and the built-in analyzers
# (hands, waveform). Depends on core / media / sys only, so sf_engine can link it.
#
# ONNX Runtime is optional. SF_ORT_DIR (an extracted onnxruntime-win-x64 release: include/ and lib/) defaults to
# C:/dev/onnxruntime-win-x64-1.30.0 when that exists. Found: SF_HAVE_ORT is defined, onnxruntime.dll is copied next to the executables
# (System32 carries an older onnxruntime.dll that would otherwise win on some setups). Not found: everything builds, ML jobs report "unavailable".
set(_ort_default "")
if(EXISTS "C:/dev/onnxruntime-win-x64-1.30.0/include/onnxruntime_cxx_api.h")
  set(_ort_default "C:/dev/onnxruntime-win-x64-1.30.0")
endif()
set(SF_ORT_DIR "${_ort_default}" CACHE PATH "ONNX Runtime release directory (include/, lib/); empty disables ML jobs")

add_library(sf_jobs STATIC
  ort_runtime.h ort_runtime.cpp
  job_scheduler.h job_scheduler.cpp
  hands_tracker.h hands_tracker.cpp
  builtin_analyzers.h builtin_analyzers.cpp
)
target_include_directories(sf_jobs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sf_jobs PUBLIC sf_core sf_media Qt6::Core Qt6::Gui PRIVATE sf_options)

if(SF_ORT_DIR AND EXISTS "${SF_ORT_DIR}/include/onnxruntime_cxx_api.h" AND EXISTS "${SF_ORT_DIR}/lib/onnxruntime.lib")
  message(STATUS "ONNX Runtime: ${SF_ORT_DIR}")
  target_compile_definitions(sf_jobs PRIVATE SF_HAVE_ORT)
  target_include_directories(sf_jobs SYSTEM PRIVATE "${SF_ORT_DIR}/include")
  target_link_libraries(sf_jobs PRIVATE "${SF_ORT_DIR}/lib/onnxruntime.lib")
  # every executable lands in CMAKE_RUNTIME_OUTPUT_DIRECTORY: one copy there serves them all, built before anything that links sf_jobs
  add_custom_target(sf_ort_dlls
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SF_ORT_DIR}/lib/onnxruntime.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMENT "Copying onnxruntime.dll next to the executables")
  if(EXISTS "${SF_ORT_DIR}/lib/onnxruntime_providers_shared.dll")
    add_custom_command(TARGET sf_ort_dlls POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SF_ORT_DIR}/lib/onnxruntime_providers_shared.dll" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  endif()
  add_dependencies(sf_jobs sf_ort_dlls)
else()
  message(STATUS "ONNX Runtime not found (set SF_ORT_DIR): ML jobs will report unavailable")
endif()
