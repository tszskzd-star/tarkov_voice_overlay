set(TVO_CLIENT_SOURCES
    src/main.cpp
    src/audio/VoiceEngine.cpp
    src/core/Application.cpp
    src/core/VoiceSession.cpp
    src/net/DirectVoiceTransport.cpp
    src/net/LanDiscovery.cpp
    src/net/SignalingClient.cpp
    src/platform/AudioSettingsWindow.cpp
    src/platform/HotkeyManager.cpp
    src/platform/OverlayWindow.cpp
    src/platform/SetupWindow.cpp
    src/platform/TrayIcon.cpp
    src/security/PasswordGate.cpp
)

if(WIN32)
    add_executable(tarkov_voice_overlay WIN32 ${TVO_CLIENT_SOURCES})
else()
    add_executable(tarkov_voice_overlay ${TVO_CLIENT_SOURCES})
endif()

target_compile_features(tarkov_voice_overlay PRIVATE cxx_std_20)
target_include_directories(tarkov_voice_overlay PRIVATE include)

if(WIN32)
    target_compile_definitions(tarkov_voice_overlay PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        TVO_PLATFORM_WINDOWS=1)
    target_link_libraries(tarkov_voice_overlay PRIVATE
        comctl32
        gdi32
        ksuser
        ole32
        winmm
        winhttp
        user32
        shell32
        ws2_32)
endif()

if(TVO_ENABLE_NATIVE_BACKENDS)
    target_compile_definitions(tarkov_voice_overlay PRIVATE TVO_ENABLE_NATIVE_BACKENDS=1)
endif()

if(TVO_BUILD_TESTS)
    add_executable(tvo_core_tests
        tests/voice_session_tests.cpp
        src/core/VoiceSession.cpp
        src/security/PasswordGate.cpp)
    target_compile_features(tvo_core_tests PRIVATE cxx_std_20)
    target_include_directories(tvo_core_tests PRIVATE include)

    add_test(NAME tvo_core_tests COMMAND tvo_core_tests)
endif()
