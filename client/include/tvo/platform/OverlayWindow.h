#pragma once

#include "tvo/core/Types.h"

#include <array>
#include <string>
#include <vector>

namespace tvo {

struct OverlayRow {
    std::string nick;
    int iconIndex = 0;
    bool muted = false;
    bool speaking = false;
    float micLevel = 0.0f;
    std::array<float, kSpectrumBands> spectrum{};
};

class OverlayWindow {
public:
    bool create();
    void show();
    void hide();
    void setRows(std::vector<OverlayRow> rows);
    void render();
    void requestClose() noexcept;

    [[nodiscard]] bool visible() const noexcept;
    [[nodiscard]] bool closeRequested() const noexcept;
    [[nodiscard]] const std::vector<OverlayRow>& rows() const noexcept;

private:
    bool created_ = false;
    bool visible_ = false;
    bool closeRequested_ = false;
    void* nativeHandle_ = nullptr;
    std::vector<OverlayRow> rows_;
};

std::vector<OverlayRow> makeOverlayRows(const std::vector<PeerStatus>& peers);

}  // namespace tvo
