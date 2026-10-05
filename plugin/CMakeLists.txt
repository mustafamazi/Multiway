# CMake build of the Multiway plugin, used for Windows (GitHub Actions) builds.
# The macOS release is still built from Multiway.jucer (see scripts/package_mac.sh);
# keep the settings below in sync with it.
#
#   cmake -S plugin -B build -A x64
#   cmake --build build --config Release
#
# JUCE is fetched from GitHub. To build against a local copy instead, pass
# -DFETCHCONTENT_SOURCE_DIR_JUCE=$HOME/JUCE

cmake_minimum_required(VERSION 3.22)

# The version lives in Multiway.jucer (version="1.1"), so both builds report the same one.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Multiway.jucer" MULTIWAY_JUCER)
string(REGEX MATCH "<JUCERPROJECT[^>]* version=\"([0-9.]+)\"" _ "${MULTIWAY_JUCER}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "Plugin version not found in Multiway.jucer")
endif()
set(MULTIWAY_VERSION "${CMAKE_MATCH_1}")

project(Multiway VERSION ${MULTIWAY_VERSION})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime so users don't need the VC++ Redistributable.
# Must be set before JUCE is added so it applies to all targets.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

if(APPLE)
    set(MULTIWAY_FORMATS VST3 AU Standalone)
else()
    set(MULTIWAY_FORMATS VST3 Standalone)
endif()

# Manufacturer and plugin codes must match Multiway.jucer: changing them breaks saved sessions.
juce_add_plugin(Multiway
    COMPANY_NAME "BopsAudio"
    BUNDLE_ID com.bopsaudio.multiway
    VERSION ${MULTIWAY_VERSION}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE Manu
    PLUGIN_CODE L2av
    FORMATS ${MULTIWAY_FORMATS}
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS com.apple.security.device.audio-input
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Multiway needs audio input to process sound in standalone mode."
    PRODUCT_NAME "Multiway")

juce_generate_juce_header(Multiway)

juce_add_binary_data(MultiwayBinaryData
    SOURCES
        Shared/Fonts/RobotoMono-VariableFont_wght.ttf
        Shared/Fonts/SpaceMono-Bold.ttf)

target_sources(Multiway
    PRIVATE
        Source/BandSpectrumComponent.cpp
        Source/PluginEditor.cpp
        Source/PluginProcessor.cpp
        Shared/BrandBadge.cpp
        Shared/ClipotypeLookAndFeel.cpp
        Shared/PillToggleButton.cpp)

target_include_directories(Multiway
    PRIVATE
        Source
        Shared)

target_compile_definitions(Multiway
    PUBLIC
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

target_link_libraries(Multiway
    PRIVATE
        MultiwayBinaryData
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
