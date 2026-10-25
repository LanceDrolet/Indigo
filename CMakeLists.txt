cmake_minimum_required(VERSION 3.24)
project(Indigo VERSION 2026.10.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4   # bump to the latest 8.x release
    GIT_SHALLOW    TRUE
    PATCH_COMMAND  ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/cmake/PatchJuce.cmake)

# Pitch-preserving time-stretch (MIT licensed, header-only; pulls in its own FFT dependency)
FetchContent_Declare(signalsmith-stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG        1.4.0
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(JUCE signalsmith-stretch)

juce_add_gui_app(Indigo PRODUCT_NAME "Indigo")
juce_generate_juce_header(Indigo)

target_sources(Indigo PRIVATE
    Source/Main.cpp
    Source/LooperEngine.cpp
    Source/LooperEngine.h
    Source/LoopStretch.h
    Source/PadGrid.h
    Source/MainComponent.h
    Source/MidiPresets.h
    Source/SettingsWindow.h
    Source/Session.h
    Source/Plugins.h
    Source/PluginManagerWindow.h
    Source/Effects.h
    Source/PadEffects.h
    Source/EffectsWindow.h
    Source/FileBrowser.h
    Source/OutputBuses.h
    Source/Mixer.h
    Source/MixerPanel.h
    Source/Panes.h)

target_compile_definitions(Indigo PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_JACK=1                 # JACK device type (served by PipeWire's JACK on most current distros)
    JUCE_JACK_CLIENT_NAME="Indigo"   # the name shown in patchbays (JUCE's default is "JUCEJack")
    JUCE_PLUGINHOST_VST3=1      # plugin hosting (the VST2 SDK is no longer licensed, so no VST2)
    JUCE_PLUGINHOST_LV2=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Indigo,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Indigo,JUCE_VERSION>")

target_link_libraries(Indigo
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_gui_basics
        signalsmith-stretch
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Third-party headers: don't hold them to our warning level
target_include_directories(Indigo SYSTEM PRIVATE
        $<TARGET_PROPERTY:signalsmith-stretch,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:signalsmith-linear,INTERFACE_INCLUDE_DIRECTORIES>)