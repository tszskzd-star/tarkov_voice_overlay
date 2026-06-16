#include "tvo/core/Application.h"

#include <cstdlib>

#ifdef TVO_PLATFORM_WINDOWS
#include <windows.h>
#endif

int main() {
#ifdef TVO_PLATFORM_WINDOWS
    HANDLE singleInstance = nullptr;
    if (std::getenv("TVO_ALLOW_MULTIPLE_INSTANCES") == nullptr) {
        singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\TarkovVoiceOverlay.SingleInstance");
    }
    if (singleInstance != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(singleInstance);
        return 0;
    }
#endif

    tvo::Application app;
    const int result = app.run();

#ifdef TVO_PLATFORM_WINDOWS
    if (singleInstance != nullptr) {
        ReleaseMutex(singleInstance);
        CloseHandle(singleInstance);
    }
#endif
    return result;
}
