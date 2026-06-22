#pragma once

namespace tvo {

class TrayIcon {
public:
    ~TrayIcon();

    bool create();
    void destroy();
    bool consumeSettingsRequest() noexcept;
    void requestSettings() noexcept;
    void requestExit() noexcept;

    [[nodiscard]] bool created() const noexcept;
    [[nodiscard]] bool exitRequested() const noexcept;

private:
    void* nativeHandle_ = nullptr;
    void* iconHandle_ = nullptr;
    bool ownsIcon_ = false;
    bool created_ = false;
    bool settingsRequested_ = false;
    bool exitRequested_ = false;
};

}  // namespace tvo
