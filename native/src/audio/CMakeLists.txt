# Audio engine: timeline renderer, mixer graph, built-in effects, loudness, CLAP host, sound card output.
# CPU only. Third-party sources (MIT / public domain) live in ../../third_party, see its README.
set(SF_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")

add_library(sf_audio STATIC
  timebase.h
  envelope.h envelope.cpp
  source.h source.cpp
  biquad.h
  effects.h effects.cpp
  loudness.h loudness.cpp
  engine.h engine.cpp
  clap_host.h clap_host.cpp
  audio_output.h audio_output.cpp
  preview_audio.h preview_audio.cpp
  miniaudio_impl.cpp
)
target_include_directories(sf_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(sf_audio SYSTEM PRIVATE "${SF_THIRD_PARTY}/miniaudio" "${SF_THIRD_PARTY}/clap/include")
# miniaudio: WASAPI only, no decoders/encoders/engine; every translation unit must see the same macros
target_compile_definitions(sf_audio PRIVATE
  MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER
  MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_WASAPI)
target_link_libraries(sf_audio PUBLIC sf_core sf_media Qt6::Core PRIVATE sf_options)
if(MSVC)
  target_compile_options(sf_audio PRIVATE /external:W0)
  set_source_files_properties(miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "/W0;/WX-")
endif()
