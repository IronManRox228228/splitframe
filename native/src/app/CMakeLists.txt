set_source_files_properties(Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(splitframe WIN32 main.cpp)
qt_add_qml_module(splitframe
  URI SplitFrame
  VERSION 1.0
  QML_FILES Main.qml Theme.qml Btn.qml Toasts.qml MediaPanel.qml PreviewPanel.qml InspectorPanel.qml TimelinePanel.qml
  SOURCES
    player.h player.cpp
    preview_item.h preview_item.cpp
    editor.h editor.cpp
    timeline_view.h timeline_view.cpp
    pool_image_provider.h
)
target_include_directories(splitframe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(splitframe PRIVATE sf_core sf_media sf_render sf_editor sf_options Qt6::Quick Qt6::QuickControls2 Qt6::GuiPrivate)
target_compile_definitions(splitframe PRIVATE PROJECT_VERSION_STRING="${PROJECT_VERSION}")

# FFmpeg DLLs next to the exe; Qt's own DLLs are deployed with windeployqt (see native/README.md)
add_custom_command(TARGET splitframe POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:splitframe>"
  COMMAND_EXPAND_LISTS)
target_link_libraries(splitframe PRIVATE sf_audio)
