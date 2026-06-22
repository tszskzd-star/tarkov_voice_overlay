#include "tvo/platform/AudioSettingsWindow.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#ifdef TVO_PLATFORM_WINDOWS
#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>

#ifndef TBS_TRANSPARENTBKGND
#define TBS_TRANSPARENTBKGND 0x1000
#endif
#ifndef VK_XBUTTON1
#define VK_XBUTTON1 0x05
#endif
#ifndef VK_XBUTTON2
#define VK_XBUTTON2 0x06
#endif
#ifndef XBUTTON1
#define XBUTTON1 0x0001
#endif
#ifndef XBUTTON2
#define XBUTTON2 0x0002
#endif
#endif

namespace tvo {

#ifdef TVO_PLATFORM_WINDOWS
namespace {

constexpr wchar_t kSettingsClassName[] = L"TarkovVoiceAudioSettingsWindow";
constexpr int kComboInputDevice = 501;
constexpr int kSliderSensitivity = 502;
constexpr int kSliderMicVolume = 503;
constexpr int kSensitivityValue = 504;
constexpr int kVolumeValue = 505;
constexpr int kRadioVoiceActivation = 506;
constexpr int kRadioPushToTalk = 507;
constexpr int kButtonPushToTalkKey = 508;
constexpr int kButtonSave = 509;
constexpr int kButtonCancel = 510;

constexpr COLORREF kColorWindow = RGB(18, 25, 32);
constexpr COLORREF kColorPanel = RGB(28, 37, 46);
constexpr COLORREF kColorText = RGB(235, 242, 248);
constexpr COLORREF kColorMutedText = RGB(153, 170, 181);
constexpr COLORREF kColorAccent = RGB(74, 211, 190);

struct InputDeviceChoice {
    std::string nameUtf8;
    std::wstring label;
};

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }

    std::string out(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), required, nullptr, nullptr);
    return out;
}

std::vector<InputDeviceChoice> enumerateInputDevices() {
    std::vector<InputDeviceChoice> devices;
    devices.push_back(InputDeviceChoice{
        .nameUtf8 = {},
        .label = L"Системный микрофон по умолчанию"});

    const UINT count = waveInGetNumDevs();
    for (UINT index = 0; index < count; ++index) {
        WAVEINCAPSW caps{};
        if (waveInGetDevCapsW(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
            continue;
        }
        std::wstring name = caps.szPname;
        if (name.empty()) {
            name = L"Микрофон " + std::to_wstring(index + 1);
        }
        devices.push_back(InputDeviceChoice{
            .nameUtf8 = toUtf8(name),
            .label = name});
    }
    return devices;
}

int sensitivityFromThreshold(float threshold) {
    const float raw = (0.085f - threshold) / 0.0008f;
    return std::clamp(static_cast<int>(raw + 0.5f), 0, 100);
}

float thresholdFromSensitivity(int sensitivity) {
    return std::clamp(0.085f - static_cast<float>(sensitivity) * 0.0008f, 0.005f, 0.085f);
}

int volumeFromGain(float gain) {
    return std::clamp(static_cast<int>(gain * 100.0f + 0.5f), 0, 200);
}

float gainFromVolume(int volume) {
    return std::clamp(static_cast<float>(volume) / 100.0f, 0.0f, 2.0f);
}

std::wstring percentText(int value) {
    return std::to_wstring(value) + L"%";
}

std::wstring virtualKeyLabel(int virtualKey) {
    if (virtualKey == VK_XBUTTON1) {
        return L"Mouse 4";
    }
    if (virtualKey == VK_XBUTTON2) {
        return L"Mouse 5";
    }
    if (virtualKey == VK_MBUTTON) {
        return L"Middle Mouse";
    }

    UINT scanCode = MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC);
    LONG lparam = static_cast<LONG>(scanCode << 16);
    if (virtualKey == VK_LEFT || virtualKey == VK_UP || virtualKey == VK_RIGHT || virtualKey == VK_DOWN ||
        virtualKey == VK_INSERT || virtualKey == VK_DELETE || virtualKey == VK_HOME || virtualKey == VK_END ||
        virtualKey == VK_PRIOR || virtualKey == VK_NEXT || virtualKey == VK_DIVIDE || virtualKey == VK_NUMLOCK) {
        lparam |= 1 << 24;
    }

    wchar_t name[64]{};
    if (scanCode != 0 &&
        GetKeyNameTextW(lparam, name, static_cast<int>(sizeof(name) / sizeof(name[0]))) > 0) {
        return name;
    }
    if (virtualKey >= 'A' && virtualKey <= 'Z') {
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    }
    return L"VK " + std::to_wstring(virtualKey);
}

void setControlFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

}  // namespace

struct AudioSettingsWindow::NativeWindow {
    bool show(const AudioSettings& settings, ApplyCallback applyCallback) {
        settings_ = settings;
        applyCallback_ = std::move(applyCallback);

        if (hwnd_ == nullptr) {
            registerClass();

            INITCOMMONCONTROLSEX controls{};
            controls.dwSize = sizeof(INITCOMMONCONTROLSEX);
            controls.dwICC = ICC_BAR_CLASSES;
            InitCommonControlsEx(&controls);

            hwnd_ = CreateWindowExW(
                WS_EX_APPWINDOW,
                kSettingsClassName,
                L"Настройки голоса",
                WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                540,
                440,
                nullptr,
                nullptr,
                GetModuleHandleW(nullptr),
                this);
            if (hwnd_ == nullptr) {
                return false;
            }
            centerWindow();
        }

        loadSettings(settings_);
        visible_ = true;
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        UpdateWindow(hwnd_);
        return true;
    }

    void destroy() {
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    [[nodiscard]] bool visible() const noexcept {
        return visible_;
    }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<NativeWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<NativeWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }

        if (self != nullptr) {
            return self->handle(hwnd, message, wparam, lparam);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            createResources();
            createControls(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint();
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wparam), kColorText);
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        case WM_COMMAND:
            handleCommand(LOWORD(wparam), HIWORD(wparam));
            return 0;
        case WM_HSCROLL:
            readSliders();
            updateSliderLabels();
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (capturePushToTalkKey(static_cast<int>(wparam))) {
                return 0;
            }
            break;
        case WM_MBUTTONDOWN:
            if (capturePushToTalkKey(VK_MBUTTON)) {
                return 0;
            }
            break;
        case WM_XBUTTONDOWN:
            if (capturePushToTalkKey(HIWORD(wparam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2)) {
                return 0;
            }
            break;
        case WM_CLOSE:
            visible_ = false;
            ShowWindow(hwnd_, SW_HIDE);
            return 0;
        case WM_DESTROY:
            destroyResources();
            hwnd_ = nullptr;
            visible_ = false;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void createResources() {
        titleFont_ = CreateFontW(-24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        uiFont_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        labelFont_ = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        backgroundBrush_ = CreateSolidBrush(kColorWindow);
    }

    void destroyResources() {
        if (titleFont_ != nullptr) {
            DeleteObject(titleFont_);
            titleFont_ = nullptr;
        }
        if (uiFont_ != nullptr) {
            DeleteObject(uiFont_);
            uiFont_ = nullptr;
        }
        if (labelFont_ != nullptr) {
            DeleteObject(labelFont_);
            labelFont_ = nullptr;
        }
        if (backgroundBrush_ != nullptr) {
            DeleteObject(backgroundBrush_);
            backgroundBrush_ = nullptr;
        }
    }

    void paint() const {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        if (hdc == nullptr) {
            return;
        }

        RECT rc{};
        GetClientRect(hwnd_, &rc);
        FillRect(hdc, &rc, backgroundBrush_);

        RECT panel{18, 74, rc.right - 18, rc.bottom - 22};
        HBRUSH panelBrush = CreateSolidBrush(kColorPanel);
        HPEN panelPen = CreatePen(PS_SOLID, 1, RGB(67, 87, 99));
        HGDIOBJ oldBrush = SelectObject(hdc, panelBrush);
        HGDIOBJ oldPen = SelectObject(hdc, panelPen);
        RoundRect(hdc, panel.left, panel.top, panel.right, panel.bottom, 16, 16);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(panelPen);
        DeleteObject(panelBrush);

        SetBkMode(hdc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(hdc, titleFont_);
        SetTextColor(hdc, kColorText);
        RECT titleRect{28, 20, rc.right - 28, 48};
        DrawTextW(hdc, L"Настройки голоса", -1, &titleRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(hdc, uiFont_);
        SetTextColor(hdc, kColorMutedText);
        RECT hintRect{30, 48, rc.right - 28, 70};
        DrawTextW(hdc, L"Микрофон и режим передачи применяются сразу после сохранения",
            -1, &hintRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(hdc, oldFont);
        EndPaint(hwnd_, &ps);
    }

    void createControls(HWND hwnd) {
        auto label = [this, hwnd](const wchar_t* text, int x, int y, int width) {
            HWND control = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
                x, y, width, 22, hwnd, nullptr, nullptr, nullptr);
            setControlFont(control, labelFont_);
            return control;
        };

        label(L"Устройство ввода", 42, 100, 180);
        inputDeviceCombo_ = CreateWindowW(
            L"COMBOBOX",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            42,
            126,
            450,
            220,
            hwnd,
            reinterpret_cast<HMENU>(kComboInputDevice),
            nullptr,
            nullptr);
        setControlFont(inputDeviceCombo_, uiFont_);

        label(L"Чувствительность", 42, 174, 170);
        sensitivitySlider_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_NOTICKS | TBS_TRANSPARENTBKGND,
            214, 166, 222, 34, hwnd, reinterpret_cast<HMENU>(kSliderSensitivity), nullptr, nullptr);
        SendMessageW(sensitivitySlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        sensitivityValue_ = CreateWindowW(L"STATIC", L"0%",
            WS_CHILD | WS_VISIBLE, 448, 174, 54, 22, hwnd,
            reinterpret_cast<HMENU>(kSensitivityValue), nullptr, nullptr);
        setControlFont(sensitivityValue_, labelFont_);

        label(L"Громкость микрофона", 42, 220, 170);
        volumeSlider_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_NOTICKS | TBS_TRANSPARENTBKGND,
            214, 212, 222, 34, hwnd, reinterpret_cast<HMENU>(kSliderMicVolume), nullptr, nullptr);
        SendMessageW(volumeSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
        volumeValue_ = CreateWindowW(L"STATIC", L"0%",
            WS_CHILD | WS_VISIBLE, 448, 220, 54, 22, hwnd,
            reinterpret_cast<HMENU>(kVolumeValue), nullptr, nullptr);
        setControlFont(volumeValue_, labelFont_);

        label(L"Режим ввода", 42, 266, 170);
        voiceRadio_ = CreateWindowW(L"BUTTON", L"Активация по голосу",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP,
            214, 264, 190, 24, hwnd, reinterpret_cast<HMENU>(kRadioVoiceActivation), nullptr, nullptr);
        setControlFont(voiceRadio_, uiFont_);
        pttRadio_ = CreateWindowW(L"BUTTON", L"Push-to-talk",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
            214, 292, 160, 24, hwnd, reinterpret_cast<HMENU>(kRadioPushToTalk), nullptr, nullptr);
        setControlFont(pttRadio_, uiFont_);

        label(L"Кнопка PTT", 42, 332, 170);
        pushToTalkKeyButton_ = CreateWindowW(L"BUTTON", virtualKeyLabel(pushToTalkVirtualKey_).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            214, 324, 170, 34, hwnd, reinterpret_cast<HMENU>(kButtonPushToTalkKey), nullptr, nullptr);
        setControlFont(pushToTalkKeyButton_, uiFont_);

        HWND save = CreateWindowW(L"BUTTON", L"Сохранить",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            286, 374, 104, 34, hwnd, reinterpret_cast<HMENU>(kButtonSave), nullptr, nullptr);
        setControlFont(save, labelFont_);
        HWND cancel = CreateWindowW(L"BUTTON", L"Закрыть",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            404, 374, 88, 34, hwnd, reinterpret_cast<HMENU>(kButtonCancel), nullptr, nullptr);
        setControlFont(cancel, labelFont_);
    }

    void loadSettings(const AudioSettings& settings) {
        devices_ = enumerateInputDevices();
        SendMessageW(inputDeviceCombo_, CB_RESETCONTENT, 0, 0);

        int selectedDevice = 0;
        for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
            SendMessageW(inputDeviceCombo_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(devices_[static_cast<std::size_t>(i)].label.c_str()));
            if (!settings.inputDeviceName.empty() &&
                devices_[static_cast<std::size_t>(i)].nameUtf8 == settings.inputDeviceName) {
                selectedDevice = i;
            }
        }
        SendMessageW(inputDeviceCombo_, CB_SETCURSEL, selectedDevice, 0);

        sensitivity_ = sensitivityFromThreshold(settings.vadThreshold);
        micVolume_ = volumeFromGain(settings.inputGain);
        pushToTalkVirtualKey_ = settings.pushToTalkVirtualKey;

        SendMessageW(sensitivitySlider_, TBM_SETPOS, TRUE, sensitivity_);
        SendMessageW(volumeSlider_, TBM_SETPOS, TRUE, micVolume_);
        SendMessageW(voiceRadio_, BM_SETCHECK, settings.pushToTalk ? BST_UNCHECKED : BST_CHECKED, 0);
        SendMessageW(pttRadio_, BM_SETCHECK, settings.pushToTalk ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(pushToTalkKeyButton_, virtualKeyLabel(pushToTalkVirtualKey_).c_str());
        updateSliderLabels();
        updateModeControls();
    }

    void handleCommand(int id, int notifyCode) {
        if (id == kButtonCancel) {
            visible_ = false;
            ShowWindow(hwnd_, SW_HIDE);
            return;
        }
        if (id == kButtonSave) {
            applyAndHide();
            return;
        }
        if (id == kButtonPushToTalkKey) {
            SendMessageW(pttRadio_, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(voiceRadio_, BM_SETCHECK, BST_UNCHECKED, 0);
            updateModeControls();
            capturingPushToTalkKey_ = true;
            SetWindowTextW(pushToTalkKeyButton_, L"Нажмите клавишу...");
            SetFocus(hwnd_);
            return;
        }
        if (id == kRadioVoiceActivation || id == kRadioPushToTalk) {
            updateModeControls();
            return;
        }
        if (id == kComboInputDevice && notifyCode == CBN_SELCHANGE) {
            return;
        }
    }

    void readSliders() {
        if (sensitivitySlider_ != nullptr) {
            sensitivity_ = static_cast<int>(SendMessageW(sensitivitySlider_, TBM_GETPOS, 0, 0));
        }
        if (volumeSlider_ != nullptr) {
            micVolume_ = static_cast<int>(SendMessageW(volumeSlider_, TBM_GETPOS, 0, 0));
        }
    }

    void updateSliderLabels() const {
        SetWindowTextW(sensitivityValue_, percentText(sensitivity_).c_str());
        SetWindowTextW(volumeValue_, percentText(micVolume_).c_str());
    }

    void updateModeControls() const {
        const bool pushToTalk =
            SendMessageW(pttRadio_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        EnableWindow(pushToTalkKeyButton_, pushToTalk);
    }

    bool capturePushToTalkKey(int virtualKey) {
        if (!capturingPushToTalkKey_ || virtualKey <= 0) {
            return false;
        }

        pushToTalkVirtualKey_ = virtualKey;
        capturingPushToTalkKey_ = false;
        SetWindowTextW(pushToTalkKeyButton_, virtualKeyLabel(pushToTalkVirtualKey_).c_str());
        return true;
    }

    void applyAndHide() {
        readSliders();
        AudioSettings updated = settings_;
        updated.vadThreshold = thresholdFromSensitivity(sensitivity_);
        updated.inputGain = gainFromVolume(micVolume_);
        updated.pushToTalk =
            SendMessageW(pttRadio_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        updated.pushToTalkVirtualKey = pushToTalkVirtualKey_;

        const int deviceIndex = static_cast<int>(SendMessageW(inputDeviceCombo_, CB_GETCURSEL, 0, 0));
        if (deviceIndex >= 0 && deviceIndex < static_cast<int>(devices_.size())) {
            updated.inputDeviceName = devices_[static_cast<std::size_t>(deviceIndex)].nameUtf8;
        } else {
            updated.inputDeviceName.clear();
        }

        settings_ = updated;
        if (applyCallback_) {
            applyCallback_(updated);
        }

        visible_ = false;
        ShowWindow(hwnd_, SW_HIDE);
    }

    void centerWindow() const {
        RECT rc{};
        GetWindowRect(hwnd_, &rc);
        const int width = rc.right - rc.left;
        const int height = rc.bottom - rc.top;
        const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }

    static void registerClass() {
        static bool registered = false;
        if (registered) {
            return;
        }

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kSettingsClassName;
        RegisterClassExW(&wc);
        registered = true;
    }

    HWND hwnd_ = nullptr;
    HWND inputDeviceCombo_ = nullptr;
    HWND sensitivitySlider_ = nullptr;
    HWND volumeSlider_ = nullptr;
    HWND sensitivityValue_ = nullptr;
    HWND volumeValue_ = nullptr;
    HWND voiceRadio_ = nullptr;
    HWND pttRadio_ = nullptr;
    HWND pushToTalkKeyButton_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT uiFont_ = nullptr;
    HFONT labelFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    AudioSettings settings_{};
    ApplyCallback applyCallback_;
    std::vector<InputDeviceChoice> devices_;
    int sensitivity_ = 60;
    int micVolume_ = 100;
    int pushToTalkVirtualKey_ = 0x56;
    bool capturingPushToTalkKey_ = false;
    bool visible_ = false;
};
#endif

AudioSettingsWindow::~AudioSettingsWindow() {
    close();
}

bool AudioSettingsWindow::show(const AudioSettings& settings, ApplyCallback applyCallback) {
#ifdef TVO_PLATFORM_WINDOWS
    if (native_ == nullptr) {
        native_ = new NativeWindow();
    }
    return native_->show(settings, std::move(applyCallback));
#else
    (void)settings;
    (void)applyCallback;
    return false;
#endif
}

void AudioSettingsWindow::close() {
#ifdef TVO_PLATFORM_WINDOWS
    if (native_ != nullptr) {
        native_->destroy();
        delete native_;
        native_ = nullptr;
    }
#endif
}

bool AudioSettingsWindow::visible() const noexcept {
#ifdef TVO_PLATFORM_WINDOWS
    return native_ != nullptr && native_->visible();
#else
    return false;
#endif
}

}  // namespace tvo
