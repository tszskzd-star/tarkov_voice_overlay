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

if(TVO_ENABLE_OPUS)
    find_package(Opus CONFIG QUIET)
    if(Opus_FOUND)
        if(TARGET Opus::opus)
            target_link_libraries(tarkov_voice_overlay PRIVATE Opus::opus)
        elseif(TARGET opus)
            target_link_libraries(tarkov_voice_overlay PRIVATE opus)
        elseif(TARGET unofficial::opus::opus)
            target_link_libraries(tarkov_voice_overlay PRIVATE unofficial::opus::opus)
        endif()
        target_compile_definitions(tarkov_voice_overlay PRIVATE TVO_HAVE_OPUS=1)
    else()
        include(FetchContent)
        set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
        set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(tvo_opus
            URL https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz
            TLS_VERIFY FALSE
            URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1)
        FetchContent_MakeAvailable(tvo_opus)
        target_link_libraries(tarkov_voice_overlay PRIVATE opus)
        target_compile_definitions(tarkov_voice_overlay PRIVATE TVO_HAVE_OPUS=1)
    endif()
endif()

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

    if(MINGW)
        get_filename_component(TVO_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach(TVO_RUNTIME_DLL
            libgcc_s_seh-1.dll
            libssp-0.dll
            libstdc++-6.dll
            libwinpthread-1.dll)
            if(EXISTS "${TVO_MINGW_BIN_DIR}/${TVO_RUNTIME_DLL}")
                add_custom_command(TARGET tarkov_voice_overlay POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${TVO_MINGW_BIN_DIR}/${TVO_RUNTIME_DLL}"
                        "$<TARGET_FILE_DIR:tarkov_voice_overlay>/${TVO_RUNTIME_DLL}"
                    VERBATIM)
            endif()
        endforeach()
    endif()
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
