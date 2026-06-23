cmake_minimum_required(VERSION 3.24)

project(TarkovVoiceOverlay
    VERSION 0.1.0
    DESCRIPTION "Low-latency squad voice overlay for Windows"
    LANGUAGES C CXX)

option(TVO_ENABLE_NATIVE_BACKENDS "Enable Dear ImGui, DirectX 11, PortAudio, Opus, RNNoise, libnice, and libsodium integrations" OFF)
option(TVO_ENABLE_OPUS "Enable the Opus voice codec" ON)
option(TVO_BUILD_TESTS "Build core smoke tests" ON)

if(TVO_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(client)
