cmake_minimum_required(VERSION 3.22)

project(LiveVoice VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
# Universal binary (Apple Silicon + Intel) on macOS
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# JUCE: use a local checkout if JUCE_DIR is given, otherwise download it.
# ---------------------------------------------------------------------------
set(JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (optional)")
if(JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.10
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

option(LIVEVOICE_BUILD_TESTS "Build offline DSP tests and UI snapshot tool" OFF)

juce_add_plugin(LiveVoice
    COMPANY_NAME "LiveVoice Audio"
    PRODUCT_NAME "LiveVoice"
    BUNDLE_ID "com.livevoice.livevoice"
    PLUGIN_MANUFACTURER_CODE Lvoi
    PLUGIN_CODE Lvc1
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES "Fx" "EQ" "Dynamics"
    COPY_PLUGIN_AFTER_BUILD FALSE)

set(LIVEVOICE_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/PresetManager.cpp
    Source/dsp/VoiceAnalysis.cpp
    Source/dsp/FeedbackSuppressor.cpp
    Source/dsp/FeedbackDetector.cpp
    Source/ui/RtaDisplay.cpp
    Source/ui/Panels.cpp)

target_sources(LiveVoice PRIVATE ${LIVEVOICE_SOURCES})
target_include_directories(LiveVoice PRIVATE Source)

target_compile_definitions(LiveVoice PUBLIC
    LIVEVOICE_VERSION="${PROJECT_VERSION}"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(LiveVoice
    PRIVATE
        juce::juce_audio_processors
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# GPU drawing (OpenGL) on Windows and macOS: much lower CPU for the meters and RTA
if(WIN32 OR APPLE)
    target_link_libraries(LiveVoice PRIVATE juce::juce_opengl)
    target_compile_definitions(LiveVoice PUBLIC LIVEVOICE_USE_OPENGL=1)
endif()

if(LIVEVOICE_BUILD_TESTS)
    add_subdirectory(Tests)
endif()
