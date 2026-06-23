#include "tvo/platform/OverlayWindow.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <utility>

#ifdef TVO_PLATFORM_WINDOWS
#include <windows.h>
#include <windowsx.h>
#endif

namespace tvo {

#ifdef TVO_PLATFORM_WINDOWS
namespace {

constexpr wchar_t kOverlayClassName[] = L"TarkovVoiceOverlayWindow";
constexpr COLORREF kTransparentColor = RGB(8, 10, 12);
constexpr int kOverlayWidth = 230;
constexpr int kOverlayMinHeight = 64;
constexpr int kOverlayRowHeight = 48;
constexpr int kCloseSize = 18;

const wchar_t* iconGlyph(int iconIndex) {
    static constexpr const wchar_t* icons[] = {
        L"\xD83D\xDC31",
        L"\xD83E\xDD8A",
        L"\xD83D\xDC3B",
        L"\xD83D\xDC3A",
        L"\xD83E\xDD89",
    };
    if (iconIndex < 0 || iconIndex >= 5) {
        return icons[0];
    }
    return icons[iconIndex];
}

COLORREF iconColor(int iconIndex) {
    static constexpr COLORREF colors[] = {
        RGB(92, 173, 226),
        RGB(235, 128, 64),
        RGB(170, 126, 82),
        RGB(127, 140, 141),
        RGB(125, 90, 181),
    };
    if (iconIndex < 0 || iconIndex >= 5) {
        return colors[0];
    }
    return colors[iconIndex];
}

void placeOnLeftSide(HWND hwnd, int rowCount) {
    const int height = std::max(kOverlayMinHeight, 24 + rowCount * kOverlayRowHeight);
    const int x = 24;
    const int y = std::max(24, (GetSystemMetrics(SM_CYSCREEN) - height) / 2);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, kOverlayWidth, height, SWP_NOACTIVATE);
}

RECT closeRectFor(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return RECT{rc.right - kCloseSize - 8, 8, rc.right - 8, 8 + kCloseSize};
}

bool pointInRect(POINT point, RECT rect) {
    return point.x >= rect.left && point.x < rect.right &&
        point.y >= rect.top && point.y < rect.bottom;
}

std::wstring toWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        wide.data(), required);
    return wide;
}

void paintOverlay(HWND hwnd, OverlayWindow* overlay) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    if (hdc == nullptr) {
        return;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);

    HBRUSH background = CreateSolidBrush(kTransparentColor);
    FillRect(hdc, &rc, background);
    DeleteObject(background);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(235, 242, 250));

    HFONT font = CreateFontW(
        -16,
        0,
        0,
        0,
        FW_MEDIUM,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        VARIABLE_PITCH,
        L"Segoe UI");
    HGDIOBJ previousFont = SelectObject(hdc, font);

    RECT closeRect = closeRectFor(hwnd);
    HPEN closePen = CreatePen(PS_SOLID, 2, RGB(230, 238, 246));
    HGDIOBJ previousPen = SelectObject(hdc, closePen);
    MoveToEx(hdc, closeRect.left + 4, closeRect.top + 4, nullptr);
    LineTo(hdc, closeRect.right - 4, closeRect.bottom - 4);
    MoveToEx(hdc, closeRect.right - 4, closeRect.top + 4, nullptr);
    LineTo(hdc, closeRect.left + 4, closeRect.bottom - 4);
    SelectObject(hdc, previousPen);
    DeleteObject(closePen);

    int y = 14;
    for (const auto& row : overlay->rows()) {
        const float level = row.muted ? 0.0f : std::clamp(row.micLevel, 0.0f, 1.0f);
        RECT ringRect{12, y - 6, 50, y + 32};
        RECT iconRect{18, y, 44, y + 26};

        HPEN ringBasePen = CreatePen(PS_SOLID, 2, row.muted ? RGB(135, 62, 62) : RGB(70, 78, 86));
        HGDIOBJ oldRingPen = SelectObject(hdc, ringBasePen);
        HGDIOBJ oldRingBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Ellipse(hdc, ringRect.left, ringRect.top, ringRect.right, ringRect.bottom);
        SelectObject(hdc, oldRingBrush);
        SelectObject(hdc, oldRingPen);
        DeleteObject(ringBasePen);

        if (!row.muted && (row.speaking || level > 0.02f)) {
            const int ringWidth = std::clamp(2 + static_cast<int>(level * 5.0f), 2, 7);
            const int green = std::clamp(150 + static_cast<int>(level * 95.0f), 150, 245);
            const int blue = std::clamp(105 + static_cast<int>(level * 70.0f), 105, 175);
            HPEN activePen = CreatePen(PS_SOLID, ringWidth, RGB(76, green, blue));
            oldRingPen = SelectObject(hdc, activePen);
            oldRingBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Ellipse(hdc, ringRect.left, ringRect.top, ringRect.right, ringRect.bottom);
            SelectObject(hdc, oldRingBrush);
            SelectObject(hdc, oldRingPen);
            DeleteObject(activePen);
        }

        HBRUSH iconBrush = CreateSolidBrush(iconColor(row.iconIndex));
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, iconBrush));
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, GetStockObject(NULL_PEN)));
        Ellipse(hdc, iconRect.left, iconRect.top, iconRect.right, iconRect.bottom);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(iconBrush);

        HFONT iconFont = CreateFontW(
            -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI Emoji");
        HGDIOBJ previousIconFont = SelectObject(hdc, iconFont);
        DrawTextW(hdc, iconGlyph(row.iconIndex), -1, &iconRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, previousIconFont);
        DeleteObject(iconFont);

        std::wstring line = toWide(row.nick);
        SIZE textSize{};
        GetTextExtentPoint32W(hdc, line.c_str(), static_cast<int>(line.size()), &textSize);

        const RECT closeRect = closeRectFor(hwnd);
        const int nameLeft = 58;
        const int maxNameRight = y < 42 ? closeRect.left - 6 : rc.right - 12;
        const int preferredNameRight = nameLeft + textSize.cx + 28;
        RECT namePill{
            nameLeft,
            y - 4,
            std::clamp(preferredNameRight, nameLeft + 68, maxNameRight),
            y + 30};

        HBRUSH nameBrush = CreateSolidBrush(row.muted ? RGB(40, 33, 37) : RGB(22, 47, 52));
        HPEN namePen = CreatePen(PS_SOLID, 1, row.muted ? RGB(122, 73, 78) : RGB(74, 211, 190));
        HGDIOBJ oldNameBrush = SelectObject(hdc, nameBrush);
        HGDIOBJ oldNamePen = SelectObject(hdc, namePen);
        RoundRect(hdc, namePill.left, namePill.top, namePill.right, namePill.bottom, 28, 28);
        SelectObject(hdc, oldNamePen);
        SelectObject(hdc, oldNameBrush);
        DeleteObject(namePen);
        DeleteObject(nameBrush);

        RECT nameText{
            namePill.left + 13,
            namePill.top,
            namePill.right - 13,
            namePill.bottom};
        SetTextColor(hdc, row.muted ? RGB(190, 150, 154) : RGB(235, 242, 250));
        DrawTextW(hdc, line.c_str(), -1, &nameText,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        y += kOverlayRowHeight;
    }

    SelectObject(hdc, previousFont);
    DeleteObject(font);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }

    if (message == WM_NCHITTEST) {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &point);
        if (pointInRect(point, closeRectFor(hwnd))) {
            return HTCLIENT;
        }
        return HTTRANSPARENT;
    }

    if (message == WM_LBUTTONUP) {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (pointInRect(point, closeRectFor(hwnd))) {
            auto* overlay = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (overlay != nullptr) {
                overlay->requestClose();
            }
            return 0;
        }
    }

    if (message == WM_ERASEBKGND) {
        return 1;
    }

    if (message == WM_PAINT) {
        auto* overlay = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (overlay != nullptr) {
            paintOverlay(hwnd, overlay);
            return 0;
        }
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void ensureOverlayClassRegistered() {
    static bool registered = false;
    if (registered) {
        return;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = overlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kOverlayClassName;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace
#endif

bool OverlayWindow::create() {
#ifdef TVO_PLATFORM_WINDOWS
    ensureOverlayClassRegistered();
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST |
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOverlayClassName,
        L"Tarkov Voice Overlay",
        WS_POPUP,
        32,
        32,
        kOverlayWidth,
        kOverlayMinHeight,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (hwnd == nullptr) {
        created_ = false;
        nativeHandle_ = nullptr;
        return false;
    }

    SetLayeredWindowAttributes(hwnd, kTransparentColor, 0, LWA_COLORKEY);
    nativeHandle_ = hwnd;
#endif
    created_ = true;
    return true;
}

void OverlayWindow::show() {
    if (created_) {
        visible_ = true;
#ifdef TVO_PLATFORM_WINDOWS
        if (nativeHandle_ != nullptr) {
            ShowWindow(static_cast<HWND>(nativeHandle_), SW_SHOWNOACTIVATE);
            placeOnLeftSide(static_cast<HWND>(nativeHandle_), static_cast<int>(rows_.size()));
        }
#endif
    }
}

void OverlayWindow::hide() {
    visible_ = false;
#ifdef TVO_PLATFORM_WINDOWS
    if (nativeHandle_ != nullptr) {
        ShowWindow(static_cast<HWND>(nativeHandle_), SW_HIDE);
    }
#endif
}

void OverlayWindow::requestClose() noexcept {
    closeRequested_ = true;
    hide();
}

void OverlayWindow::setRows(std::vector<OverlayRow> rows) {
    rows_ = std::move(rows);
#ifdef TVO_PLATFORM_WINDOWS
    if (nativeHandle_ != nullptr) {
        placeOnLeftSide(static_cast<HWND>(nativeHandle_), static_cast<int>(rows_.size()));
        InvalidateRect(static_cast<HWND>(nativeHandle_), nullptr, FALSE);
    }
#endif
}

void OverlayWindow::render() {
    if (!visible_) {
        return;
    }

#ifdef TVO_PLATFORM_WINDOWS
    if (nativeHandle_ != nullptr) {
        InvalidateRect(static_cast<HWND>(nativeHandle_), nullptr, FALSE);
        UpdateWindow(static_cast<HWND>(nativeHandle_));
    }
#endif

    std::cout << "Overlay:";
    for (const auto& row : rows_) {
        std::cout << " [" << row.nick
                  << " level=" << static_cast<int>(row.micLevel * 100.0f) << "%]";
    }
    std::cout << "\n";
}

bool OverlayWindow::visible() const noexcept {
    return visible_;
}

bool OverlayWindow::closeRequested() const noexcept {
    return closeRequested_;
}

const std::vector<OverlayRow>& OverlayWindow::rows() const noexcept {
    return rows_;
}

std::vector<OverlayRow> makeOverlayRows(const std::vector<PeerStatus>& peers) {
    std::vector<OverlayRow> rows;
    rows.reserve(peers.size());
    for (const auto& peer : peers) {
        rows.push_back(OverlayRow{
            .nick = peer.nick,
            .iconIndex = peer.iconIndex,
            .muted = peer.muted,
            .speaking = peer.speaking,
            .micLevel = std::clamp(peer.micLevel, 0.0f, 1.0f),
            .spectrum = peer.spectrum});
    }
    return rows;
}

}  // namespace tvo
