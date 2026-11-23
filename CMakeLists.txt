cmake_minimum_required(VERSION 3.22)
project(CrayLdrDrumLabV1 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(juce)

# JUCE 8.0.6 Fruity Wrapper state-stream compatibility workaround.
set(CRAY_JUCE_VST3_CLIENT "${juce_SOURCE_DIR}/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST3.cpp")
file(READ "${CRAY_JUCE_VST3_CLIENT}" CRAY_JUCE_VST3_SOURCE)
set(CRAY_JUCE_OLD_READ_BLOCK [==[                auto status = state->read (buffer, (Steinberg::int32) bytesPerBlock, &bytesRead);

                if (bytesRead <= 0 || (status != kResultTrue && ! detail::PluginUtilities::getHostType().isWavelab()))
                    break;

                allData.write (buffer, static_cast<size_t> (bytesRead));]==])
set(CRAY_JUCE_CACHED_READ_BLOCK [==[                state->read (buffer, (Steinberg::int32) bytesPerBlock, &bytesRead);

                // Some hosts may return kResultOk with a valid partial read.
                // Continue until the stream reports that no bytes remain.
                if (bytesRead <= 0)
                    break;

                allData.write (buffer, static_cast<size_t> (bytesRead));]==])
set(CRAY_JUCE_NEW_READ_BLOCK [==[                auto status = state->read (buffer, (Steinberg::int32) bytesPerBlock, &bytesRead);

                // FL Studio may report kResultOk as well as kResultTrue for a successful read.
                if (bytesRead <= 0 || (status != kResultOk && status != kResultTrue))
                    break;

                allData.write (buffer, static_cast<size_t> (bytesRead));]==])
string(FIND "${CRAY_JUCE_VST3_SOURCE}" "${CRAY_JUCE_OLD_READ_BLOCK}" CRAY_JUCE_OLD_READ_POSITION)
string(FIND "${CRAY_JUCE_VST3_SOURCE}" "${CRAY_JUCE_CACHED_READ_BLOCK}" CRAY_JUCE_CACHED_READ_POSITION)
string(FIND "${CRAY_JUCE_VST3_SOURCE}" "${CRAY_JUCE_NEW_READ_BLOCK}" CRAY_JUCE_NEW_READ_POSITION)
if(NOT CRAY_JUCE_OLD_READ_POSITION EQUAL -1)
    string(REPLACE "${CRAY_JUCE_OLD_READ_BLOCK}" "${CRAY_JUCE_NEW_READ_BLOCK}" CRAY_JUCE_VST3_SOURCE "${CRAY_JUCE_VST3_SOURCE}")
elseif(NOT CRAY_JUCE_CACHED_READ_POSITION EQUAL -1)
    string(REPLACE "${CRAY_JUCE_CACHED_READ_BLOCK}" "${CRAY_JUCE_NEW_READ_BLOCK}" CRAY_JUCE_VST3_SOURCE "${CRAY_JUCE_VST3_SOURCE}")
elseif(CRAY_JUCE_NEW_READ_POSITION EQUAL -1)
    message(FATAL_ERROR "Could not apply CRAY LDR JUCE VST3 stream compatibility patch")
endif()

set(CRAY_JUCE_OLD_SEEK_BLOCK [==[        const auto seekToBeginningOfStream = [&]
        {
            return state->seek (0, IBStream::kIBSeekSet, nullptr) == kResultTrue;
        };]==])
set(CRAY_JUCE_NEW_SEEK_BLOCK [==[        const auto seekToBeginningOfStream = [&]
        {
            const auto result = state->seek (0, IBStream::kIBSeekSet, nullptr);
            return result == kResultOk || result == kResultTrue;
        };]==])
string(FIND "${CRAY_JUCE_VST3_SOURCE}" "${CRAY_JUCE_OLD_SEEK_BLOCK}" CRAY_JUCE_OLD_SEEK_POSITION)
string(FIND "${CRAY_JUCE_VST3_SOURCE}" "${CRAY_JUCE_NEW_SEEK_BLOCK}" CRAY_JUCE_NEW_SEEK_POSITION)
if(NOT CRAY_JUCE_OLD_SEEK_POSITION EQUAL -1)
    string(REPLACE "${CRAY_JUCE_OLD_SEEK_BLOCK}" "${CRAY_JUCE_NEW_SEEK_BLOCK}" CRAY_JUCE_VST3_SOURCE "${CRAY_JUCE_VST3_SOURCE}")
elseif(CRAY_JUCE_NEW_SEEK_POSITION EQUAL -1)
    message(FATAL_ERROR "Could not apply CRAY LDR JUCE VST3 seek compatibility patch")
endif()

file(WRITE "${CRAY_JUCE_VST3_CLIENT}" "${CRAY_JUCE_VST3_SOURCE}")


juce_add_plugin(CrayLdrDrumLabV1
    COMPANY_NAME "Cray LDR"
    BUNDLE_ID "com.crayldr.drumlab.v1"
    PLUGIN_MANUFACTURER_CODE CrLd
    PLUGIN_CODE DrL1
    PRODUCT_NAME "CRAY LDR DRUM LAB V1"
    VST3_CATEGORIES Drum
    DESCRIPTION "Drum sampler and transient-slicing instrument - CRAY LDR DRUM LAB V1 identity"
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
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
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
target_link_libraries(CrayLdrDrumLabV1 PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
include(CTest)
if(BUILD_TESTING)
    add_executable(DrumkitOnsetDetectorTests Tests/DrumkitOnsetDetectorTests.cpp)
    target_compile_features(DrumkitOnsetDetectorTests PRIVATE cxx_std_17)
    target_include_directories(DrumkitOnsetDetectorTests PRIVATE Source)
    add_test(NAME DrumkitOnsetDetectorTests COMMAND DrumkitOnsetDetectorTests)
endif()
