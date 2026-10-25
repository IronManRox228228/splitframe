set_source_files_properties(Theme.qml Icons.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(splitframe WIN32 main.cpp)
qt_add_qml_module(splitframe
  URI SplitFrame
  VERSION 1.0
  QML_FILES Main.qml Theme.qml Btn.qml Toasts.qml MediaPanel.qml PreviewPanel.qml InspectorPanel.qml TimelinePanel.qml ExportDialog.qml
    EditPage.qml ColourPage.qml AudioPage.qml PageSwitcher.qml
    GlassPanel.qml Pill.qml GlassCombo.qml GlassSlider.qml Fader.qml LevelMeter.qml
    Icons.qml Icon.qml IconBtn.qml GButton.qml GInput.qml GDialog.qml GlassMenu.qml DashedBox.qml Spinner.qml
    HomePage.qml ProjectCard.qml LeftRail.qml AssistantPanel.qml AgentChip.qml AgentModePill.qml ToolRow.qml ReviewCard.qml PlanCard.qml ConfirmCard.qml
    Markdown.qml StoryboardStrip.qml CommandBar.qml AiSettingsDialog.qml RefractiveGlass.qml
    format.js agentfmt.js
  RESOURCES assets/splitframe-logo.png fonts/Geist-Variable.woff2 fonts/GeistMono-Variable.woff2 fonts/OFL.txt
  SOURCES
    player.h player.cpp
    preview_item.h preview_item.cpp
    editor.h editor.cpp
    export_controller.h export_controller.cpp
    timeline_view.h timeline_view.cpp
    pool_image_provider.h
    window_recorder.h window_recorder.cpp
    mixer_model.h mixer_model.cpp
    colour_page.h colour_page.cpp
)
# refraction glass (RefractiveGlass.qml): compiled to .qsb for every backend Qt Quick may pick
qt_add_shaders(splitframe "glass_shaders" PREFIX "/qt/qml/SplitFrame" FILES shaders/refract.frag)
target_include_directories(splitframe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(splitframe PRIVATE sf_core sf_media sf_render sf_editor sf_options Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::GuiPrivate)
target_compile_definitions(splitframe PRIVATE PROJECT_VERSION_STRING="${PROJECT_VERSION}")

# FFmpeg DLLs next to the exe; Qt's own DLLs are deployed with windeployqt (see native/README.md)
add_custom_command(TARGET splitframe POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:splitframe>"
  COMMAND_EXPAND_LISTS)
target_link_libraries(splitframe PRIVATE sf_audio sf_export sf_engine sf_ai)
