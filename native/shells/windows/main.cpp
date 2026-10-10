#include "bridge_native/client.hpp"

#define UNICODE
#include <windows.h>

#include <cstdlib>
#include <string>

namespace {
std::string env_value(const char* name) { const char* value = std::getenv(name); return value ? value : ""; }
std::wstring wide(const std::string& value) { return std::wstring(value.begin(), value.end()); }
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_COMMAND && LOWORD(wparam) == 1) {
        bridge_native::ClientOptions options{env_value("BRIDGEPANEL_URL"), env_value("BRIDGEPANEL_TOKEN"), {5000, true}};
        bridge_native::BridgePanelClient client(options);
        const std::string machine = env_value("BRIDGEPANEL_MACHINE");
        const auto capabilities = client.capabilities();
        const auto peers = client.peers();
        const auto sessions = client.sessions(machine);
        std::string text;
        if (capabilities.ok() && peers.ok() && sessions.ok()) {
            text = bridge_native::Json{{"capabilities", capabilities.value()}, {"peers", peers.value()}, {"sessions", sessions.value()}}.dump(2);
        } else {
            const auto& error = !capabilities.ok() ? capabilities.error() : (!peers.ok() ? peers.error() : sessions.error());
            text = error.code + ": " + error.message;
        }
        SetWindowTextW(window, wide(text).c_str());
        return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const wchar_t klass[] = L"BridgeNativePhase0";
    WNDCLASSW window_class{}; window_class.hInstance = instance; window_class.lpfnWndProc = window_proc; window_class.lpszClassName = klass;
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, klass, L"BridgeSessions Native", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 760, 480, nullptr, nullptr, instance, nullptr);
    CreateWindowW(L"BUTTON", L"Refresh peers/sessions", WS_VISIBLE | WS_CHILD,
                  20, 20, 220, 32, window, reinterpret_cast<HMENU>(1), instance, nullptr);
    ShowWindow(window, show);
    MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return static_cast<int>(message.wParam);
}
