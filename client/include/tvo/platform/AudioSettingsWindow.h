#pragma once

#include "tvo/core/Types.h"

#include <functional>

namespace tvo {

class AudioSettingsWindow {
public:
    using ApplyCallback = std::function<void(const AudioSettings&)>;

    ~AudioSettingsWindow();

    bool show(const AudioSettings& settings, ApplyCallback applyCallback);
    void close();

    [[nodiscard]] bool visible() const noexcept;

private:
    struct NativeWindow;
    NativeWindow* native_ = nullptr;
};

}  // namespace tvo
