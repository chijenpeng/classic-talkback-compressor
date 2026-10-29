cmake_minimum_required(VERSION 3.22)

project(ClassicTalkbackCompressor VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
option(COPY_PLUGIN_AFTER_BUILD "Copy AU and VST3 bundles after building the plug-in target" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The DSP: the reduced-circuit card engine. No JUCE dependency, so it is built and tested offline.
# No -ffast-math: the solves rely on IEEE semantics.
file(GLOB_RECURSE LMC_DSP_SOURCES CONFIGURE_DEPENDS Source/dsp/*.cpp)
add_library(lmc_dsp STATIC ${LMC_DSP_SOURCES})
target_include_directories(lmc_dsp PUBLIC Source/dsp PRIVATE models/devices)
if(MSVC)
    target_compile_options(lmc_dsp PRIVATE /O2 /W4)
else()
    target_compile_options(lmc_dsp PRIVATE -O3 -Wall -Wextra)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(Source/dsp/Card.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=on;-fno-math-errno")
endif()

add_executable(lmc_test tests/lmc_test.cpp)
target_link_libraries(lmc_test PRIVATE lmc_dsp)
if(MSVC)
    target_compile_options(lmc_test PRIVATE /O2 /W4)
else()
    target_compile_options(lmc_test PRIVATE -O3 -Wall -Wextra)
endif()
target_compile_definitions(lmc_test PRIVATE LMC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/reference/fixtures")

enable_testing()
add_test(NAME lmc_test COMMAND lmc_test)

# ---------------------------------------------------------------------------------------------
# PLUG-IN: the JUCE wrapper and editor around lmc_dsp (two lmc::Card per engine set for
# stereo), its fonts, the processor test and the snapshot tool.
# ---------------------------------------------------------------------------------------------
# Prefer a local JUCE checkout for day-to-day development (tools/link-juce.sh). A fresh clone remains
# buildable: when third_party/JUCE is absent CMake fetches the exact release used by this project.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
    add_subdirectory(third_party/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(ClassicTalkback
    PRODUCT_NAME "Classic Talkback Compressor"
    COMPANY_NAME "codycodie"
    BUNDLE_ID "com.c0dypeng.classictalkbackcompressor"
    PLUGIN_MANUFACTURER_CODE Cdpg
    PLUGIN_CODE CtCp
    FORMATS AU VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Dynamics
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD ${COPY_PLUGIN_AFTER_BUILD})

# Inter and JetBrains Mono, both SIL Open Font License (licences are next to the fonts).
juce_add_binary_data(ClassicTalkbackAssets SOURCES
    Resources/fonts/Inter-SemiBold.ttf
    Resources/fonts/JetBrainsMono-Medium.ttf)

target_sources(ClassicTalkback PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(ClassicTalkback PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(ClassicTalkback
    PRIVATE
        lmc_dsp
        ClassicTalkbackAssets
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Processor regression: parameters, state, layouts, latency, Mix, stereo modes, transitions, allocations, sample rates.
juce_add_console_app(lmc_processor_test)
target_sources(lmc_processor_test PRIVATE
    tests/lmc_processor_test.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
target_compile_definitions(lmc_processor_test PRIVATE
    JucePlugin_Name="Classic Talkback Compressor"
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(lmc_processor_test
    PRIVATE
        lmc_dsp
        ClassicTalkbackAssets
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Renders the editor to docs/editor.png (and the 0.75x and 2x scale variants) and checks the hover help.
# Run it from the repository root.
juce_add_console_app(lmc_snapshot)
target_sources(lmc_snapshot PRIVATE
    tests/snapshot.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
target_compile_definitions(lmc_snapshot PRIVATE
    JucePlugin_Name="Classic Talkback Compressor"
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(lmc_snapshot
    PRIVATE
        lmc_dsp
        ClassicTalkbackAssets
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

add_test(NAME lmc_processor_test COMMAND lmc_processor_test)
