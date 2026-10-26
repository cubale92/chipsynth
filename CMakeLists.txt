cmake_minimum_required(VERSION 3.22)
project(SFC8 VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

# JUCE: use a local checkout if JUCE_DIR is given, otherwise download it
if(DEFINED JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

set(SFC8_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND SFC8_FORMATS AU)
endif()

juce_add_plugin(SFC8
    COMPANY_NAME "8cee"
    COMPANY_WEBSITE ""
    PRODUCT_NAME "SFC-8"
    PLUGIN_MANUFACTURER_CODE Ecee
    PLUGIN_CODE Sfc8
    FORMATS ${SFC8_FORMATS}
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Synth
    AU_MAIN_TYPE kAudioUnitType_MusicDevice)

target_sources(SFC8 PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Snes/Engine.cpp
    Source/Snes/Loaders.cpp)

target_compile_definitions(SFC8 PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(SFC8
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Core DSP test (no JUCE): cmake --build . --target dsp_test
add_executable(dsp_test EXCLUDE_FROM_ALL tests/dsp_test.cpp Source/Snes/Engine.cpp Source/Snes/Loaders.cpp)

# Headless plugin test (loads files, plays chords, state round-trip, editor snapshot)
add_executable(plugin_test EXCLUDE_FROM_ALL tests/plugin_test.cpp)
target_link_libraries(plugin_test PRIVATE SFC8)
target_include_directories(plugin_test PRIVATE $<TARGET_PROPERTY:SFC8,INCLUDE_DIRECTORIES>)
target_compile_definitions(plugin_test PRIVATE $<TARGET_PROPERTY:SFC8,COMPILE_DEFINITIONS>)
