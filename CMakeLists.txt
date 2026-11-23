cmake_minimum_required(VERSION 3.22)

project(CrayLdrDrumLabV1 VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(CrayLdrDrumLabV1
    COMPANY_NAME "CRAY LDR"
    BUNDLE_ID "com.crayldr.drumlab.v1"
    PLUGIN_MANUFACTURER_CODE CrLd
    PLUGIN_CODE DrL1
    PRODUCT_NAME "CRAY LDR DRUM LAB V1"
    DESCRIPTION "64-pad drum sampler and transient slicer"
    VST3_CATEGORIES Instrument|Drum
    FORMATS VST3
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_generate_juce_header(CrayLdrDrumLabV1)

target_sources(CrayLdrDrumLabV1 PRIVATE
    PluginProcessor.cpp
    PluginProcessor.h
    PluginEditor.cpp
    PluginEditor.h
    DrumLabEngine.h
)

target_compile_features(CrayLdrDrumLabV1 PRIVATE cxx_std_17)
target_compile_definitions(CrayLdrDrumLabV1 PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)
if(MSVC)
    target_compile_options(CrayLdrDrumLabV1 PRIVATE /utf-8)
endif()

target_link_libraries(CrayLdrDrumLabV1
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
