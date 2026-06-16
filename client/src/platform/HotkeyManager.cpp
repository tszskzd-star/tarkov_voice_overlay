#include "tvo/platform/HotkeyManager.h"

#include <utility>

#ifdef TVO_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace tvo {

bool HotkeyManager::registerMuteHotkey(const HotkeySettings& settings, Callback callback) {
    settings_ = settings;
    muteCallback_ = std::move(callback);

#ifdef TVO_PLATFORM_WINDOWS
    UINT modifiers = 0;
    if (settings_.ctrl) {
        modifiers |= MOD_CONTROL;
    }
    if (settings_.shift) {
        modifiers |= MOD_SHIFT;
    }
    if (settings_.alt) {
        modifiers |= MOD_ALT;
    }

    registered_ = RegisterHotKey(nullptr, 1, modifiers | MOD_NOREPEAT, settings_.muteVirtualKey) != 0;
    return registered_;
#else
    registered_ = true;
    return true;
#endif
}

void HotkeyManager::unregisterAll() {
#ifdef TVO_PLATFORM_WINDOWS
    if (registered_) {
        UnregisterHotKey(nullptr, 1);
    }
#endif
    registered_ = false;
    muteCallback_ = nullptr;
}

void HotkeyManager::poll() {
#ifdef TVO_PLATFORM_WINDOWS
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_HOTKEY && message.wParam == 1 && muteCallback_) {
            muteCallback_();
        } else {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
#endif
}

bool HotkeyManager::isKeyDown(int virtualKey) const {
#ifdef TVO_PLATFORM_WINDOWS
    if (virtualKey <= 0) {
        return false;
    }
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
#else
    (void)virtualKey;
    return false;
#endif
}

void HotkeyManager::triggerMuteForSmokeTest() {
    if (registered_ && muteCallback_) {
        muteCallback_();
    }
}

}  // namespace tvo
