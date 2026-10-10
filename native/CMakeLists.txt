cmake_minimum_required(VERSION 3.25)
project(SplitFrame VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Every binary (app, tests) lands in one folder next to the Qt and FFmpeg DLLs it needs.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(SF_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(SF_SANITIZE_ADDRESS "Build with AddressSanitizer" OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network Quick QuickControls2 ShaderTools Test)
find_package(FFmpeg REQUIRED)
# OpenColorIO 2.x is optional: without it the compositor keeps its built-in sRGB / BT.709 / PQ / HLG
# colour pipeline and only OCIO config spaces, views and LUT files are unavailable (see render/color_manager.h).
# scripts/dev.cmd sets OCIO_ROOT (default C:\dev\ocio, built per native/README.md).
set(OCIO_ROOT "$ENV{OCIO_ROOT}" CACHE PATH "OpenColorIO install prefix")
find_package(OpenColorIO 2 CONFIG QUIET HINTS "${OCIO_ROOT}")
if(OpenColorIO_FOUND)
  message(STATUS "OpenColorIO ${OpenColorIO_VERSION}: ${OpenColorIO_DIR}")
else()
  message(WARNING "OpenColorIO 2.x not found (set OCIO_ROOT to its install prefix): building without colour management; "
                  "config colour spaces, display/views and LUT files will be unavailable")
endif()
qt_standard_project_setup(REQUIRES 6.8)

add_library(sf_options INTERFACE)
if(MSVC)
  target_compile_options(sf_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  if(SF_WARNINGS_AS_ERRORS)
    target_compile_options(sf_options INTERFACE /WX)
  endif()
  if(SF_SANITIZE_ADDRESS)
    target_compile_options(sf_options INTERFACE /fsanitize=address)
  endif()
else()
  target_compile_options(sf_options INTERFACE -Wall -Wextra -Wpedantic)
  if(SF_WARNINGS_AS_ERRORS)
    target_compile_options(sf_options INTERFACE -Werror)
  endif()
  if(SF_SANITIZE_ADDRESS)
    target_compile_options(sf_options INTERFACE -fsanitize=address)
    target_link_options(sf_options INTERFACE -fsanitize=address)
  endif()
endif()

add_subdirectory(src/core)
add_subdirectory(src/media)
add_subdirectory(src/render)
add_subdirectory(src/editor)
add_subdirectory(src/app)

enable_testing()
add_subdirectory(tests)
add_subdirectory(bench)

# Audio engine (mixer, effects, loudness, output device, CLAP host). Defined last so existing targets keep their order.
add_subdirectory(src/audio)

# Export and proxies (render + audio + media). sf_editor's media pool links it for proxy generation.
add_subdirectory(src/export)

# Headless engine, local API, MCP server and the splitframe-cli executable. Last: it builds on every library above.
add_subdirectory(src/engine)
