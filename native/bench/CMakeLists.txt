# Decode benchmark: not a test, run it by hand (prefer the release preset for real numbers).
#   scripts\dev.cmd cmake --preset release -B build/media-rel
#   scripts\dev.cmd cmake --build build/media-rel --target bench_decode
#   scripts\dev.cmd build\media-rel\bin\bench_decode.exe
qt_add_executable(bench_decode bench_decode.cpp)
target_link_libraries(bench_decode PRIVATE sf_media sf_options Qt6::Core Qt6::Gui)
target_compile_definitions(bench_decode PRIVATE SF_FFMPEG_EXE_DEFAULT="${FFMPEG_EXECUTABLE}")
