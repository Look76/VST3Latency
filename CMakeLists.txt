cmake_minimum_required(VERSION 3.22)
project(VST3Latency VERSION 1.0.0 LANGUAGES C CXX)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.8
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(VST3Latency
    PRODUCT_NAME "VST3 Latency"
    BUNDLE_ID com.vst3latency.desktop)
target_sources(VST3Latency PRIVATE Main.cpp)
target_compile_features(VST3Latency PRIVATE cxx_std_17)
target_compile_definitions(VST3Latency PRIVATE
    JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_USE_CAMERA=0 JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(VST3Latency PRIVATE
    juce::juce_audio_processors juce::juce_gui_basics
    juce::juce_recommended_config_flags)
