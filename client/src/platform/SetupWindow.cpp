#include "tvo/platform/SetupWindow.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

constexpr wchar_t kSetupClassName[] = L"TarkovVoiceSetupWindow";
constexpr int kEditNick = 101;
constexpr int kButtonStart = 102;
constexpr int kIconBase = 200;
constexpr int kSliderSensitivity = 301;
constexpr int kSliderMicVolume = 302;
constexpr int kButtonMicTest = 303;
constexpr int kSensitivityValue = 304;
constexpr int kVolumeValue = 305;
constexpr int kTestStatus = 306;
constexpr int kCheckPushToTalk = 307;
constexpr int kButtonPushToTalkKey = 308;

constexpr COLORREF kColorWindowTop = RGB(14, 19, 25);
constexpr COLORREF kColorWindowBottom = RGB(20, 29, 36);
constexpr COLORREF kColorPanel = RGB(28, 37, 46);
constexpr COLORREF kColorPanelSoft = RGB(33, 44, 54);
constexpr COLORREF kColorBorder = RGB(67, 87, 99);
constexpr COLORREF kColorText = RGB(235, 242, 248);
constexpr COLORREF kColorMutedText = RGB(153, 170, 181);
constexpr COLORREF kColorAccent = RGB(74, 211, 190);
constexpr COLORREF kColorAccentDark = RGB(20, 132, 123);
constexpr COLORREF kColorEdit = RGB(18, 25, 32);

struct IconChoice {
    const wchar_t* glyph;
    const wchar_t* name;
    COLORREF color;
};

constexpr std::array<IconChoice, 5> kIcons{{
    {L"\xD83D\xDC31", L"Кот", RGB(92, 173, 226)},
    {L"\xD83E\xDD8A", L"Лиса", RGB(235, 128, 64)},
    {L"\xD83D\xDC3B", L"Медведь", RGB(170, 126, 82)},
    {L"\xD83D\xDC3A", L"Волк", RGB(127, 140, 141)},
    {L"\xD83E\xDD89", L"Сова", RGB(125, 90, 181)},
}};

COLORREF blendColor(COLORREF from, COLORREF to, float t) {
    const auto blend = [t](int a, int b) {
        return static_cast<int>(static_cast<float>(a) + static_cast<float>(b - a) * t);
    };
    return RGB(
        blend(GetRValue(from), GetRValue(to)),
        blend(GetGValue(from), GetGValue(to)),
        blend(GetBValue(from), GetBValue(to)));
}

void fillVerticalGradient(HDC hdc, RECT rect, COLORREF top, COLORREF bottom) {
    const int height = std::max(1, static_cast<int>(rect.bottom - rect.top));
    for (int y = rect.top; y < rect.bottom; ++y) {
        const float t = static_cast<float>(y - rect.top) / static_cast<float>(height);
        HPEN pen = CreatePen(PS_SOLID, 1, blendColor(top, bottom, t));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        MoveToEx(hdc, rect.left, y, nullptr);
        LineTo(hdc, rect.right, y);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }
}

void drawRoundRect(
    HDC hdc,
    RECT rect,
    int radius,
    COLORREF fill,
    COLORREF border,
    int borderWidth = 1) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, borderWidth, border);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

std::wstring toWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        wide.data(), required);
    return wide;
}

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        utf8.data(), required, nullptr, nullptr);
    return utf8;
}

std::string trimNick(std::string nick) {
    auto notSpace = [](unsigned char c) { return c > ' '; };
    nick.erase(nick.begin(), std::find_if(nick.begin(), nick.end(), notSpace));
    nick.erase(std::find_if(nick.rbegin(), nick.rend(), notSpace).base(), nick.end());
    if (nick.empty()) {
        return "Player";
    }
    if (nick.size() > 24) {
        nick.resize(24);
    }
    return nick;
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

bool recordTwoSeconds(std::vector<std::int16_t>& samples, std::wstring& error) {
    constexpr int sampleRate = 16000;
    constexpr int seconds = 2;

    samples.assign(sampleRate * seconds, 0);

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = sampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        error = L"Не удалось создать событие записи.";
        return false;
    }

    HWAVEIN input = nullptr;
    MMRESULT mm = waveInOpen(&input, WAVE_MAPPER, &format,
        reinterpret_cast<DWORD_PTR>(event), 0, CALLBACK_EVENT);
    if (mm != MMSYSERR_NOERROR) {
        CloseHandle(event);
        error = L"Микрофон недоступен. Проверьте устройство ввода Windows.";
        return false;
    }

    WAVEHDR header{};
    header.lpData = reinterpret_cast<LPSTR>(samples.data());
    header.dwBufferLength = static_cast<DWORD>(samples.size() * sizeof(std::int16_t));

    mm = waveInPrepareHeader(input, &header, sizeof(WAVEHDR));
    if (mm == MMSYSERR_NOERROR) {
        mm = waveInAddBuffer(input, &header, sizeof(WAVEHDR));
    }
    if (mm == MMSYSERR_NOERROR) {
        ResetEvent(event);
        mm = waveInStart(input);
    }
    if (mm != MMSYSERR_NOERROR) {
        waveInClose(input);
        CloseHandle(event);
        error = L"Не удалось начать запись.";
        return false;
    }

    Sleep(seconds * 1000);
    waveInStop(input);
    waveInReset(input);

    const DWORD recordedBytes = header.dwBytesRecorded;
    waveInUnprepareHeader(input, &header, sizeof(WAVEHDR));
    waveInClose(input);
    CloseHandle(event);

    const std::size_t recordedSamples = recordedBytes / sizeof(std::int16_t);
    if (recordedSamples == 0) {
        samples.clear();
        error = L"No microphone data was recorded. Check the Windows input device.";
        return false;
    }
    if (recordedSamples < samples.size()) {
        samples.resize(recordedSamples);
    }
    return true;
}

int maxAbsSample(const std::vector<std::int16_t>& samples) {
    int maxAbs = 0;
    for (std::int16_t sample : samples) {
        maxAbs = std::max(maxAbs, std::abs(static_cast<int>(sample)));
    }
    return maxAbs;
}

void saveWavForDiagnostics(const std::vector<std::int16_t>& samples) {
    std::error_code ignored;
    std::filesystem::create_directories("logs", ignored);

    std::ofstream out("logs/mic-test.wav", std::ios::binary | std::ios::trunc);
    if (!out) {
        return;
    }

    constexpr std::uint32_t sampleRate = 16000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bitsPerSample = 16;
    const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint32_t riffSize = 36 + dataSize;
    const std::uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
    const std::uint16_t blockAlign = channels * bitsPerSample / 8;
    const std::uint32_t fmtSize = 16;
    const std::uint16_t audioFormat = 1;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    out.write(reinterpret_cast<const char*>(&fmtSize), sizeof(fmtSize));
    out.write(reinterpret_cast<const char*>(&audioFormat), sizeof(audioFormat));
    out.write(reinterpret_cast<const char*>(&channels), sizeof(channels));
    out.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    out.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    out.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
    out.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
    out.write(reinterpret_cast<const char*>(samples.data()), dataSize);
}

bool playRecording(std::vector<std::int16_t> samples, float gain, std::wstring& error) {
    if (samples.empty()) {
        error = L"Запись пустая.";
        return false;
    }

    const int maxAbs = maxAbsSample(samples);
    if (maxAbs < 96) {
        error = L"Recorded voice is too quiet. Check the Windows microphone input.";
        return false;
    }

    saveWavForDiagnostics(samples);
    const float autoGain = std::clamp(22000.0f / static_cast<float>(maxAbs), 1.0f, 10.0f);
    const float playbackGain = std::clamp(gain * autoGain, 0.2f, 12.0f);
    for (auto& sample : samples) {
        const int scaled = static_cast<int>(static_cast<float>(sample) * playbackGain);
        sample = static_cast<std::int16_t>(std::clamp(scaled, -32768, 32767));
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 16000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HWAVEOUT output = nullptr;
    if (waveOutOpen(&output, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        error = L"Не удалось открыть устройство вывода.";
        return false;
    }

    WAVEHDR header{};
    header.lpData = reinterpret_cast<LPSTR>(samples.data());
    header.dwBufferLength = static_cast<DWORD>(samples.size() * sizeof(std::int16_t));

    if (waveOutPrepareHeader(output, &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
        waveOutWrite(output, &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
        waveOutClose(output);
        error = L"Не удалось воспроизвести запись.";
        return false;
    }

    while ((header.dwFlags & WHDR_DONE) == 0) {
        Sleep(20);
    }

    waveOutUnprepareHeader(output, &header, sizeof(WAVEHDR));
    waveOutClose(output);
    return true;
}

class NativeSetupWindow {
public:
    explicit NativeSetupWindow(const AppSettings& defaults)
        : defaults_(defaults),
          result_{
              .accepted = false,
              .nick = defaults.nick,
              .iconIndex = defaults.iconIndex,
              .vadThreshold = defaults.audio.vadThreshold,
              .inputGain = defaults.audio.inputGain,
              .pushToTalk = defaults.audio.pushToTalk,
              .pushToTalkVirtualKey = defaults.audio.pushToTalkVirtualKey},
          sensitivity_(sensitivityFromThreshold(defaults.audio.vadThreshold)),
          micVolume_(volumeFromGain(defaults.audio.inputGain)),
          pushToTalk_(defaults.audio.pushToTalk),
          pushToTalkVirtualKey_(defaults.audio.pushToTalkVirtualKey) {}

    SetupResult show() {
        registerClass();

        INITCOMMONCONTROLSEX controls{};
        controls.dwSize = sizeof(INITCOMMONCONTROLSEX);
        controls.dwICC = ICC_BAR_CLASSES;
        InitCommonControlsEx(&controls);

        hwnd_ = CreateWindowExW(
            WS_EX_APPWINDOW,
            kSetupClassName,
            L"Tarkov Voice Overlay",
            WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            620,
            700,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            this);

        if (hwnd_ == nullptr) {
            result_.accepted = true;
            return result_;
        }

        centerWindow();
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        return result_;
    }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<NativeSetupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<NativeSetupWindow*>(create->lpCreateParams);
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
            createVisualResources();
            createControls(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paintWindow();
            return 0;
        case WM_CTLCOLORSTATIC:
            return colorStatic(reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam));
        case WM_CTLCOLOREDIT:
            return colorEdit(reinterpret_cast<HDC>(wparam));
        case WM_CTLCOLORBTN:
            return colorButton(reinterpret_cast<HDC>(wparam));
        case WM_COMMAND:
            handleCommand(LOWORD(wparam));
            return 0;
        case WM_DRAWITEM:
            drawOwnerButton(reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
            return TRUE;
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
            done_ = true;
            result_.accepted = false;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            destroyVisualResources();
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void createVisualResources() {
        titleFont_ = CreateFontW(-30, 0, 0, 0, 700, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI Variable Display");
        subtitleFont_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        uiFont_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        labelFont_ = CreateFontW(-14, 0, 0, 0, 600, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        buttonFont_ = CreateFontW(-16, 0, 0, 0, 700, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

        panelBrush_ = CreateSolidBrush(kColorPanel);
        editBrush_ = CreateSolidBrush(kColorEdit);
    }

    void destroyVisualResources() {
        if (titleFont_ != nullptr) {
            DeleteObject(titleFont_);
            titleFont_ = nullptr;
        }
        if (subtitleFont_ != nullptr) {
            DeleteObject(subtitleFont_);
            subtitleFont_ = nullptr;
        }
        if (uiFont_ != nullptr) {
            DeleteObject(uiFont_);
            uiFont_ = nullptr;
        }
        if (labelFont_ != nullptr) {
            DeleteObject(labelFont_);
            labelFont_ = nullptr;
        }
        if (buttonFont_ != nullptr) {
            DeleteObject(buttonFont_);
            buttonFont_ = nullptr;
        }
        if (panelBrush_ != nullptr) {
            DeleteObject(panelBrush_);
            panelBrush_ = nullptr;
        }
        if (editBrush_ != nullptr) {
            DeleteObject(editBrush_);
            editBrush_ = nullptr;
        }
    }

    void paintWindow() const {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        if (hdc == nullptr) {
            return;
        }

        RECT rc{};
        GetClientRect(hwnd_, &rc);
        fillVerticalGradient(hdc, rc, kColorWindowTop, kColorWindowBottom);

        RECT hero{24, 22, rc.right - 24, 112};
        drawRoundRect(hdc, hero, 18, RGB(19, 28, 36), RGB(43, 148, 137), 1);

        HBRUSH accentBrush = CreateSolidBrush(kColorAccent);
        HGDIOBJ oldBrush = SelectObject(hdc, accentBrush);
        HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, 46, 44, 92, 90);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(accentBrush);

        HPEN headsetPen = CreatePen(PS_SOLID, 4, RGB(15, 30, 36));
        HGDIOBJ oldHeadsetPen = SelectObject(hdc, headsetPen);
        Arc(hdc, 55, 52, 83, 82, 55, 69, 83, 69);
        RoundRect(hdc, 52, 67, 61, 82, 6, 6);
        RoundRect(hdc, 77, 67, 86, 82, 6, 6);
        SelectObject(hdc, oldHeadsetPen);
        DeleteObject(headsetPen);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kColorText);
        HGDIOBJ oldFont = SelectObject(hdc, titleFont_);
        RECT titleText{108, 35, rc.right - 48, 66};
        DrawTextW(hdc, L"Tarkov Voice Overlay", -1, &titleText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(hdc, subtitleFont_);
        SetTextColor(hdc, kColorMutedText);
        RECT subtitleText{110, 70, rc.right - 48, 96};
        DrawTextW(hdc, L"Настройка голоса и оверлея перед рейдом", -1, &subtitleText, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(hdc, oldFont);

        RECT identityPanel{24, 128, rc.right - 24, 292};
        RECT audioPanel{24, 312, rc.right - 24, 558};
        RECT footerPanel{24, 576, rc.right - 24, 638};
        drawRoundRect(hdc, identityPanel, 16, kColorPanel, kColorBorder, 1);
        drawRoundRect(hdc, audioPanel, 16, kColorPanel, kColorBorder, 1);
        drawRoundRect(hdc, footerPanel, 16, RGB(21, 30, 38), RGB(45, 66, 77), 1);

        SelectObject(hdc, labelFont_);
        SetTextColor(hdc, kColorAccent);
        TextOutW(hdc, 46, 142, L"Профиль", 7);
        TextOutW(hdc, 46, 326, L"Аудио", 5);

        SelectObject(hdc, oldFont);
        EndPaint(hwnd_, &ps);
    }

    LRESULT colorStatic(HDC hdc, HWND control) const {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, control == testStatus_ ? kColorAccent : kColorText);
        return reinterpret_cast<LRESULT>(panelBrush_);
    }

    LRESULT colorEdit(HDC hdc) const {
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, kColorEdit);
        SetTextColor(hdc, kColorText);
        return reinterpret_cast<LRESULT>(editBrush_);
    }

    LRESULT colorButton(HDC hdc) const {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kColorText);
        return reinterpret_cast<LRESULT>(panelBrush_);
    }

    void createControls(HWND hwnd) {
        auto setFont = [this](HWND control, HFONT font = nullptr) {
            SendMessageW(control, WM_SETFONT,
                reinterpret_cast<WPARAM>(font != nullptr ? font : uiFont_), TRUE);
        };

        HWND nickLabel = CreateWindowW(L"STATIC", L"Ник",
            WS_CHILD | WS_VISIBLE, 48, 173, 90, 22, hwnd, nullptr, nullptr, nullptr);
        setFont(nickLabel, labelFont_);

        editNick_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", toWide(defaults_.nick).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            116, 166, 260, 32, hwnd, reinterpret_cast<HMENU>(kEditNick), nullptr, nullptr);
        setFont(editNick_);

        HWND iconLabel = CreateWindowW(L"STATIC", L"Иконка",
            WS_CHILD | WS_VISIBLE, 48, 218, 90, 22, hwnd, nullptr, nullptr, nullptr);
        setFont(iconLabel, labelFont_);

        for (int i = 0; i < static_cast<int>(kIcons.size()); ++i) {
            HWND button = CreateWindowW(L"BUTTON", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                116 + i * 92, 209, 82, 62, hwnd,
                reinterpret_cast<HMENU>(static_cast<intptr_t>(kIconBase + i)),
                nullptr, nullptr);
            setFont(button);
            iconButtons_[static_cast<std::size_t>(i)] = button;
        }

        HWND sensitivityLabel = CreateWindowW(L"STATIC", L"Чувствительность",
            WS_CHILD | WS_VISIBLE, 48, 367, 150, 22, hwnd, nullptr, nullptr, nullptr);
        setFont(sensitivityLabel, labelFont_);

        sensitivitySlider_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_NOTICKS | TBS_TRANSPARENTBKGND,
            214, 358, 268, 34, hwnd, reinterpret_cast<HMENU>(kSliderSensitivity), nullptr, nullptr);
        SendMessageW(sensitivitySlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(sensitivitySlider_, TBM_SETPOS, TRUE, sensitivity_);

        sensitivityValue_ = CreateWindowW(L"STATIC", percentText(sensitivity_).c_str(),
            WS_CHILD | WS_VISIBLE, 502, 366, 58, 22, hwnd,
            reinterpret_cast<HMENU>(kSensitivityValue), nullptr, nullptr);
        setFont(sensitivityValue_, labelFont_);

        HWND volumeLabel = CreateWindowW(L"STATIC", L"Громкость микрофона",
            WS_CHILD | WS_VISIBLE, 48, 414, 160, 22, hwnd, nullptr, nullptr, nullptr);
        setFont(volumeLabel, labelFont_);

        volumeSlider_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_NOTICKS | TBS_TRANSPARENTBKGND,
            214, 405, 268, 34, hwnd, reinterpret_cast<HMENU>(kSliderMicVolume), nullptr, nullptr);
        SendMessageW(volumeSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
        SendMessageW(volumeSlider_, TBM_SETPOS, TRUE, micVolume_);

        volumeValue_ = CreateWindowW(L"STATIC", percentText(micVolume_).c_str(),
            WS_CHILD | WS_VISIBLE, 502, 413, 58, 22, hwnd,
            reinterpret_cast<HMENU>(kVolumeValue), nullptr, nullptr);
        setFont(volumeValue_, labelFont_);

        HWND test = CreateWindowW(L"BUTTON", L"Тест: запись 2 сек",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            214, 457, 182, 38, hwnd, reinterpret_cast<HMENU>(kButtonMicTest), nullptr, nullptr);
        setFont(test, buttonFont_);

        testStatus_ = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE, 416, 465, 150, 22, hwnd,
            reinterpret_cast<HMENU>(kTestStatus), nullptr, nullptr);
        setFont(testStatus_, labelFont_);

        pushToTalkCheck_ = CreateWindowW(L"BUTTON", L"Push-to-talk",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            48, 512, 160, 24, hwnd, reinterpret_cast<HMENU>(kCheckPushToTalk), nullptr, nullptr);
        setFont(pushToTalkCheck_);
        SendMessageW(pushToTalkCheck_, BM_SETCHECK, pushToTalk_ ? BST_CHECKED : BST_UNCHECKED, 0);

        HWND pushToTalkLabel = CreateWindowW(L"STATIC", L"PTT key",
            WS_CHILD | WS_VISIBLE, 245, 514, 70, 22, hwnd, nullptr, nullptr, nullptr);
        setFont(pushToTalkLabel, labelFont_);

        pushToTalkKeyButton_ = CreateWindowW(L"BUTTON", virtualKeyLabel(pushToTalkVirtualKey_).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            324, 504, 160, 38, hwnd, reinterpret_cast<HMENU>(kButtonPushToTalkKey), nullptr, nullptr);
        setFont(pushToTalkKeyButton_, buttonFont_);

        HWND hint = CreateWindowW(L"STATIC",
            L"После старта окно скроется, а ник останется в углу.",
            WS_CHILD | WS_VISIBLE, 48, 600, 360, 22, hwnd, nullptr, nullptr, nullptr);
        setFont(hint, subtitleFont_);

        HWND start = CreateWindowW(L"BUTTON", L"Старт",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            432, 590, 134, 42, hwnd, reinterpret_cast<HMENU>(kButtonStart), nullptr, nullptr);
        setFont(start, buttonFont_);
    }

    void handleCommand(int id) {
        if (id >= kIconBase && id < kIconBase + static_cast<int>(kIcons.size())) {
            result_.iconIndex = id - kIconBase;
            for (HWND button : iconButtons_) {
                InvalidateRect(button, nullptr, TRUE);
            }
            return;
        }

        if (id == kButtonMicTest) {
            runMicTest();
            return;
        }

        if (id == kButtonPushToTalkKey) {
            capturingPushToTalkKey_ = true;
            SetWindowTextW(pushToTalkKeyButton_, L"Press key...");
            SetFocus(hwnd_);
            return;
        }

        if (id == kButtonStart) {
            wchar_t buffer[128]{};
            GetWindowTextW(editNick_, buffer, static_cast<int>(sizeof(buffer) / sizeof(buffer[0])));
            readSliders();
            result_.nick = trimNick(toUtf8(buffer));
            result_.iconIndex = std::clamp(result_.iconIndex, 0, 4);
            result_.vadThreshold = thresholdFromSensitivity(sensitivity_);
            result_.inputGain = gainFromVolume(micVolume_);
            result_.pushToTalk = pushToTalkCheck_ != nullptr &&
                SendMessageW(pushToTalkCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            result_.pushToTalkVirtualKey = pushToTalkVirtualKey_;
            result_.accepted = true;
            done_ = true;
            DestroyWindow(hwnd_);
        }
    }

    bool capturePushToTalkKey(int virtualKey) {
        if (!capturingPushToTalkKey_ || virtualKey <= 0) {
            return false;
        }

        pushToTalkVirtualKey_ = virtualKey;
        result_.pushToTalkVirtualKey = virtualKey;
        capturingPushToTalkKey_ = false;
        if (pushToTalkKeyButton_ != nullptr) {
            SetWindowTextW(pushToTalkKeyButton_, virtualKeyLabel(pushToTalkVirtualKey_).c_str());
        }
        return true;
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
        if (sensitivityValue_ != nullptr) {
            SetWindowTextW(sensitivityValue_, percentText(sensitivity_).c_str());
        }
        if (volumeValue_ != nullptr) {
            SetWindowTextW(volumeValue_, percentText(micVolume_).c_str());
        }
    }

    void runMicTest() {
        readSliders();
        SetWindowTextW(testStatus_, L"Говорите...");
        EnableWindow(GetDlgItem(hwnd_, kButtonMicTest), FALSE);
        UpdateWindow(hwnd_);

        Beep(880, 160);
        Sleep(120);

        std::vector<std::int16_t> samples;
        std::wstring error;
        if (!recordTwoSeconds(samples, error)) {
            EnableWindow(GetDlgItem(hwnd_, kButtonMicTest), TRUE);
            SetWindowTextW(testStatus_, L"Ошибка записи");
            MessageBoxW(hwnd_, error.c_str(), L"Тест микрофона", MB_ICONWARNING);
            return;
        }

        SetWindowTextW(testStatus_, L"Слушаем...");
        UpdateWindow(hwnd_);
        if (!playRecording(std::move(samples), gainFromVolume(micVolume_), error)) {
            EnableWindow(GetDlgItem(hwnd_, kButtonMicTest), TRUE);
            SetWindowTextW(testStatus_, L"Ошибка вывода");
            MessageBoxW(hwnd_, error.c_str(), L"Тест микрофона", MB_ICONWARNING);
            return;
        }

        SetWindowTextW(testStatus_, L"Готово");
        EnableWindow(GetDlgItem(hwnd_, kButtonMicTest), TRUE);
    }

    void drawOwnerButton(DRAWITEMSTRUCT* item) const {
        if (item == nullptr) {
            return;
        }

        if (item->CtlID >= kIconBase &&
            item->CtlID < kIconBase + static_cast<int>(kIcons.size())) {
            drawIconButton(item);
            return;
        }

        drawActionButton(item);
    }

    void drawActionButton(DRAWITEMSTRUCT* item) const {
        HDC hdc = item->hDC;
        RECT rc = item->rcItem;
        const bool pressed = (item->itemState & ODS_SELECTED) != 0;
        const bool disabled = (item->itemState & ODS_DISABLED) != 0;
        const bool focused = (item->itemState & ODS_FOCUS) != 0;

        COLORREF fill = kColorPanelSoft;
        COLORREF border = kColorBorder;
        COLORREF text = kColorText;
        if (item->CtlID == kButtonStart) {
            fill = pressed ? kColorAccentDark : kColorAccent;
            border = RGB(116, 246, 224);
            text = RGB(6, 25, 28);
        } else if (item->CtlID == kButtonPushToTalkKey) {
            fill = pressed ? RGB(35, 55, 65) : RGB(23, 34, 42);
            border = RGB(82, 109, 123);
        } else if (pressed) {
            fill = RGB(40, 59, 69);
        }

        if (disabled) {
            fill = RGB(30, 38, 45);
            border = RGB(55, 65, 72);
            text = RGB(120, 132, 140);
        }

        RECT buttonRect{rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1};
        drawRoundRect(hdc, buttonRect, 14, fill, border, focused ? 2 : 1);

        wchar_t textBuffer[96]{};
        GetWindowTextW(item->hwndItem, textBuffer, static_cast<int>(sizeof(textBuffer) / sizeof(textBuffer[0])));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, text);
        HGDIOBJ oldFont = SelectObject(hdc, buttonFont_);
        DrawTextW(hdc, textBuffer, -1, &buttonRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
    }

    void drawIconButton(DRAWITEMSTRUCT* item) const {
        if (item == nullptr || item->CtlID < kIconBase ||
            item->CtlID >= kIconBase + static_cast<int>(kIcons.size())) {
            return;
        }

        const int index = static_cast<int>(item->CtlID) - kIconBase;
        const bool selected = index == result_.iconIndex;
        const auto& icon = kIcons[static_cast<std::size_t>(index)];
        HDC hdc = item->hDC;
        RECT rc = item->rcItem;
        const bool pressed = (item->itemState & ODS_SELECTED) != 0;
        const bool focused = (item->itemState & ODS_FOCUS) != 0;

        RECT card{rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1};
        const COLORREF fill = selected
            ? blendColor(icon.color, RGB(24, 34, 42), 0.60f)
            : (pressed ? RGB(38, 51, 60) : RGB(24, 33, 41));
        const COLORREF border = selected ? icon.color : (focused ? kColorAccent : RGB(49, 67, 78));
        drawRoundRect(hdc, card, 16, fill, border, selected || focused ? 2 : 1);

        RECT circle{rc.left + 24, rc.top + 8, rc.left + 58, rc.top + 42};
        HBRUSH iconBrush = CreateSolidBrush(icon.color);
        HGDIOBJ oldBrush = SelectObject(hdc, iconBrush);
        HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, circle.left, circle.top, circle.right, circle.bottom);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(iconBrush);

        SetBkMode(hdc, TRANSPARENT);
        HFONT emojiFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI Emoji");
        HGDIOBJ oldFont = SelectObject(hdc, emojiFont);
        DrawTextW(hdc, icon.glyph, -1, &circle, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(emojiFont);

        oldFont = SelectObject(hdc, labelFont_);
        SetTextColor(hdc, selected ? RGB(245, 250, 252) : kColorMutedText);
        RECT textRect{rc.left + 5, rc.top + 41, rc.right - 5, rc.bottom - 5};
        DrawTextW(hdc, icon.name, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
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
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kSetupClassName;
        RegisterClassExW(&wc);
        registered = true;
    }

    AppSettings defaults_;
    SetupResult result_;
    int sensitivity_ = 60;
    int micVolume_ = 100;
    bool pushToTalk_ = false;
    int pushToTalkVirtualKey_ = 0x56;
    HWND hwnd_ = nullptr;
    HWND editNick_ = nullptr;
    HWND sensitivitySlider_ = nullptr;
    HWND volumeSlider_ = nullptr;
    HWND sensitivityValue_ = nullptr;
    HWND volumeValue_ = nullptr;
    HWND testStatus_ = nullptr;
    HWND pushToTalkCheck_ = nullptr;
    HWND pushToTalkKeyButton_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT subtitleFont_ = nullptr;
    HFONT uiFont_ = nullptr;
    HFONT labelFont_ = nullptr;
    HFONT buttonFont_ = nullptr;
    HBRUSH panelBrush_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    std::array<HWND, 5> iconButtons_{};
    bool capturingPushToTalkKey_ = false;
    bool done_ = false;
};

}  // namespace
#endif

SetupResult SetupWindow::showModal(const AppSettings& defaults) {
#ifdef TVO_PLATFORM_WINDOWS
    return NativeSetupWindow(defaults).show();
#else
    return SetupResult{
        .accepted = true,
        .nick = defaults.nick,
        .iconIndex = defaults.iconIndex,
        .vadThreshold = defaults.audio.vadThreshold,
        .inputGain = defaults.audio.inputGain,
        .pushToTalk = defaults.audio.pushToTalk,
        .pushToTalkVirtualKey = defaults.audio.pushToTalkVirtualKey};
#endif
}

}  // namespace tvo
