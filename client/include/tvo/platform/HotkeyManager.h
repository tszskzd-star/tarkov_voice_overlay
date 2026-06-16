#pragma once

#include "tvo/core/Types.h"

#include <functional>

namespace tvo {

class HotkeyManager {
public:
    using Callback = std::function<void()>;

    bool registerMuteHotkey(const HotkeySettings& settings, Callback callback);
    void unregisterAll();
    void poll();
    bool isKeyDown(int virtualKey) const;
    void triggerMuteForSmokeTest();

private:
    HotkeySettings settings_{};
    Callback muteCallback_;
    bool registered_ = false;
};

}  // namespace tvo
