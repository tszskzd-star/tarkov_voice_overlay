#pragma once

#include "tvo/audio/VoiceEngine.h"
#include "tvo/core/Types.h"
#include "tvo/core/VoiceSession.h"
#include "tvo/net/DirectVoiceTransport.h"
#include "tvo/net/LanDiscovery.h"
#include "tvo/net/SignalingClient.h"
#include "tvo/platform/HotkeyManager.h"
#include "tvo/platform/OverlayWindow.h"
#include "tvo/platform/SetupWindow.h"
#include "tvo/security/PasswordGate.h"

namespace tvo {

class Application {
public:
    int run();

private:
    void handleSignalingEvent(const SignalingEvent& event);
    void updateOverlay();

    AppSettings settings_{};
    VoiceSession session_;
    VoiceEngine voice_;
    DirectVoiceTransport directVoice_;
    SignalingClient signaling_;
    LanDiscovery lan_;
    OverlayWindow overlay_;
    HotkeyManager hotkeys_;
    PasswordGate passwordGate_;
};

}  // namespace tvo
