# Multiway tests: a console app independent of the plugin (Multiway.jucer).
#
#   cmake -S plugin/Tests -B plugin/Tests/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build plugin/Tests/build
#   plugin/Tests/build/MultiwayTests_artefacts/Release/MultiwayTests

cmake_minimum_required (VERSION 3.22)
project (MultiwayTests VERSION 0.3.0)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

set (JUCE_DIR "$ENV{HOME}/JUCE" CACHE PATH "JUCE root directory (the copy the plugin uses)")
add_subdirectory (${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)

set (PLUGIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The editor gets compiled because of the processor's createEditor; it reads the fonts from BinaryData.
juce_add_binary_data (MultiwayTestBinaryData
    SOURCES
        ${PLUGIN_DIR}/Shared/Fonts/RobotoMono-VariableFont_wght.ttf
        ${PLUGIN_DIR}/Shared/Fonts/SpaceMono-Bold.ttf)

juce_add_console_app (MultiwayTests PRODUCT_NAME "MultiwayTests")
juce_generate_juce_header (MultiwayTests)

target_sources (MultiwayTests PRIVATE
    StftTests.cpp
    ${PLUGIN_DIR}/Source/PluginProcessor.cpp
    ${PLUGIN_DIR}/Source/PluginEditor.cpp
    ${PLUGIN_DIR}/Source/BandSpectrumComponent.cpp
    ${PLUGIN_DIR}/Shared/ClipotypeLookAndFeel.cpp
    ${PLUGIN_DIR}/Shared/PillToggleButton.cpp
    ${PLUGIN_DIR}/Shared/BrandBadge.cpp)

target_include_directories (MultiwayTests PRIVATE
    ${PLUGIN_DIR}/Source
    ${PLUGIN_DIR}/Shared)

# Plugin definitions the processor expects (Projucer generates these in JucePluginDefines.h).
target_compile_definitions (MultiwayTests PRIVATE
    JucePlugin_Name="Multiway"
    JucePlugin_VersionString="1.0.0"
    JucePlugin_IsSynth=0
    JucePlugin_IsMidiEffect=0
    JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1)   # the test runs the message loop by hand for the latency timer

target_link_libraries (MultiwayTests
    PRIVATE
        MultiwayTestBinaryData
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
