# Offline DSP tests + a UI snapshot tool. Not needed to build the plugin.
juce_add_console_app(LiveVoiceTests PRODUCT_NAME "LiveVoiceTests")

target_sources(LiveVoiceTests PRIVATE
    TestMain.cpp
    ${CMAKE_SOURCE_DIR}/Source/PluginProcessor.cpp
    ${CMAKE_SOURCE_DIR}/Source/PluginEditor.cpp
    ${CMAKE_SOURCE_DIR}/Source/PresetManager.cpp
    ${CMAKE_SOURCE_DIR}/Source/dsp/FeedbackSuppressor.cpp
    ${CMAKE_SOURCE_DIR}/Source/dsp/FeedbackDetector.cpp
    ${CMAKE_SOURCE_DIR}/Source/ui/RtaDisplay.cpp
    ${CMAKE_SOURCE_DIR}/Source/ui/Panels.cpp)

target_include_directories(LiveVoiceTests PRIVATE ${CMAKE_SOURCE_DIR}/Source)

target_compile_definitions(LiveVoiceTests PRIVATE
    JucePlugin_Name="LiveVoice"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(LiveVoiceTests PRIVATE
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_recommended_config_flags)
