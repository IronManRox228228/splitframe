# Decode benchmark: not a test, run it by hand (prefer the release preset for real numbers).
#   scripts\dev.cmd cmake --preset release -B build/media-rel
#   scripts\dev.cmd cmake --build build/media-rel --target bench_decode
#   scripts\dev.cmd build\media-rel\bin\bench_decode.exe
qt_add_executable(bench_decode bench_decode.cpp)
target_link_libraries(bench_decode PRIVATE sf_media sf_options Qt6::Core Qt6::Gui)
target_compile_definitions(bench_decode PRIVATE SF_FFMPEG_EXE_DEFAULT="${FFMPEG_EXECUTABLE}")

# Compositor + playback pipeline: zero-copy GPU vs CPU paths, 1080p/4K, 1 and 3 stacked layers.
#   scripts\dev.cmd cmake -S . -B build/render-rel -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=cl ...
#   scripts\dev.cmd cmake --build build/render-rel --target bench_compositor -j 4
#   scripts\dev.cmd build\render-rel\bin\bench_compositor.exe
qt_add_executable(bench_compositor bench_compositor.cpp)
target_link_libraries(bench_compositor PRIVATE sf_render sf_media sf_core sf_options Qt6::Core Qt6::Gui)
target_compile_definitions(bench_compositor PRIVATE SF_FFMPEG_EXE_DEFAULT="${FFMPEG_EXECUTABLE}")
