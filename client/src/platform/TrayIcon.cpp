#include "tvo/platform/TrayIcon.h"

#ifdef TVO_PLATFORM_WINDOWS
#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#endif

namespace tvo {

#ifdef TVO_PLATFORM_WINDOWS
namespace {

constexpr wchar_t kTrayWindowClassName[] = L"TarkovVoiceOverlayTrayWindow";
constexpr UINT kTrayMessage = WM_APP + 41;
constexpr UINT_PTR kTrayIconId = 1;
constexpr UINT kMenuSettings = 1001;
constexpr UINT kMenuExit = 1002;

std::filesystem::path executableDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(path).parent_path();
}

HICON loadTrayIcon(bool& owned) {
    owned = false;
    const auto iconPath = executableDirectory() / L"headphones.ico";
    if (std::filesystem::exists(iconPath)) {
        HICON icon = static_cast<HICON>(LoadImageW(
            nullptr,
            iconPath.c_str(),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_LOADFROMFILE));
        if (icon != nullptr) {
            owned = true;
            return icon;
        }
    }

    return LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
}

void fillNotifyData(HWND hwnd, HICON icon, NOTIFYICONDATAW& data) {
    data = NOTIFYICONDATAW{};
    data.cbSize = sizeof(NOTIFYICONDATAW);
    data.hWnd = hwnd;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = icon;
    wcscpy_s(data.szTip, L"Tarkov Voice Overlay");
}

void showTrayMenu(HWND hwnd, TrayIcon* tray) {
    if (tray == nullptr) {
        return;
    }

    POINT cursor{};
    GetCursorPos(&cursor);

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Настройки");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Выход");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
        cursor.x,
        cursor.y,
        0,
        hwnd,
        nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

LRESULT CALLBACK trayWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }

    auto* tray = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case kTrayMessage:
        if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) {
            showTrayMenu(hwnd, tray);
            return 0;
        }
        if (LOWORD(lparam) == WM_LBUTTONDBLCLK && tray != nullptr) {
            tray->requestSettings();
            return 0;
        }
        break;
    case WM_COMMAND:
        if (tray != nullptr && LOWORD(wparam) == kMenuSettings) {
            tray->requestSettings();
            return 0;
        }
        if (tray != nullptr && LOWORD(wparam) == kMenuExit) {
            tray->requestExit();
            return 0;
        }
        break;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void ensureTrayClassRegistered() {
    static bool registered = false;
    if (registered) {
        return;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = trayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kTrayWindowClassName;
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace
#endif

TrayIcon::~TrayIcon() {
    destroy();
}

bool TrayIcon::create() {
#ifdef TVO_PLATFORM_WINDOWS
    if (created_) {
        return true;
    }

    ensureTrayClassRegistered();
    HWND hwnd = CreateWindowExW(
        0,
        kTrayWindowClassName,
        L"Tarkov Voice Overlay Tray",
        WS_OVERLAPPED,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        this);
    if (hwnd == nullptr) {
        return false;
    }

    bool ownsIcon = false;
    HICON icon = loadTrayIcon(ownsIcon);
    NOTIFYICONDATAW data{};
    fillNotifyData(hwnd, icon, data);
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        DestroyWindow(hwnd);
        if (ownsIcon && icon != nullptr) {
            DestroyIcon(icon);
        }
        return false;
    }

    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);

    nativeHandle_ = hwnd;
    iconHandle_ = icon;
    ownsIcon_ = ownsIcon;
#endif
    created_ = true;
    return true;
}

void TrayIcon::destroy() {
#ifdef TVO_PLATFORM_WINDOWS
    if (nativeHandle_ != nullptr) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(NOTIFYICONDATAW);
        data.hWnd = static_cast<HWND>(nativeHandle_);
        data.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        DestroyWindow(static_cast<HWND>(nativeHandle_));
        nativeHandle_ = nullptr;
    }
    if (ownsIcon_ && iconHandle_ != nullptr) {
        DestroyIcon(static_cast<HICON>(iconHandle_));
    }
    iconHandle_ = nullptr;
    ownsIcon_ = false;
#endif
    created_ = false;
}

bool TrayIcon::consumeSettingsRequest() noexcept {
    const bool requested = settingsRequested_;
    settingsRequested_ = false;
    return requested;
}

void TrayIcon::requestSettings() noexcept {
    settingsRequested_ = true;
}

void TrayIcon::requestExit() noexcept {
    exitRequested_ = true;
}

bool TrayIcon::created() const noexcept {
    return created_;
}

bool TrayIcon::exitRequested() const noexcept {
    return exitRequested_;
}

}  // namespace tvo
