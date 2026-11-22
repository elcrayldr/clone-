cmake_minimum_required(VERSION 3.22)
project(CrayLdrDrumkitClone VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(juce)

juce_add_plugin(CrayLdrDrumkitClone
    COMPANY_NAME "CRAY LDR"
    BUNDLE_ID "com.crayldr.drumkitclone"
    PLUGIN_MANUFACTURER_CODE CrLd
    PLUGIN_CODE Drk1
    PRODUCT_NAME "CRAY LDR - DRUMKIT CLONE"
    VST3_CATEGORIES Instrument|Drum
    DESCRIPTION "Drum kit extraction and sampler - development build"
    FORMATS VST3
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_generate_juce_header(CrayLdrDrumkitClone)

target_sources(CrayLdrDrumkitClone PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_compile_features(CrayLdrDrumkitClone PRIVATE cxx_std_17)
target_compile_definitions(CrayLdrDrumkitClone PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

if(MSVC)
    target_compile_options(CrayLdrDrumkitClone PRIVATE /utf-8)
endif()

target_link_libraries(CrayLdrDrumkitClone PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

include(CTest)
if(BUILD_TESTING)
    add_executable(DrumkitOnsetDetectorTests
        Tests/DrumkitOnsetDetectorTests.cpp
    )
    target_compile_features(DrumkitOnsetDetectorTests PRIVATE cxx_std_17)
    target_include_directories(DrumkitOnsetDetectorTests PRIVATE Source)
    add_test(NAME DrumkitOnsetDetectorTests COMMAND DrumkitOnsetDetectorTests)
endif()
