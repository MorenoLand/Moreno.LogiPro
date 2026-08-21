#include "logipro/hid_capture.hpp"
#include "logipro/hid.hpp"

#ifdef _WIN32

#include <windows.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr wchar_t window_class_name[] = L"LogiProRawInputWindow";
constexpr int emergency_hotkey_id = 3;
HANDLE stop_event = nullptr;
DWORD capture_thread_id = 0;

struct CaptureState {
    std::map<std::wstring, std::vector<std::uint8_t>> previous_reports;
};

std::wstring raw_device_name(HANDLE device) {
    if (device == nullptr) return {};
    UINT character_count = 0;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &character_count) == static_cast<UINT>(-1) || character_count == 0) return {};
    std::wstring name(character_count, L'\0');
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name.data(), &character_count) == static_cast<UINT>(-1)) return {};
    name.resize(character_count);
    return name;
}

bool is_logitech_path(const std::wstring& path) {
    std::wstring upper = path;
    for (wchar_t& character : upper) {
        if (character >= L'a' && character <= L'z') character = static_cast<wchar_t>(character - (L'a' - L'A'));
    }
    return upper.find(L"VID_046D") != std::wstring::npos;
}

void print_hid_report(CaptureState& state, const std::wstring& path, const std::uint8_t* bytes, std::size_t length) {
    const std::vector<std::uint8_t> report(bytes, bytes + length);
    auto& previous = state.previous_reports[path];
    if (report == previous) return;
    std::wcout << L"[raw hid] " << path << L" bytes=" << std::dec << length << L":";
    for (const std::uint8_t byte : report) {
        std::wcout << L' ' << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(byte);
    }
    std::wcout << std::dec << std::setfill(L' ') << L'\n' << std::flush;
    previous = report;
}

void print_raw_input(CaptureState& state, HRAWINPUT input) {
    UINT byte_count = 0;
    if (GetRawInputData(input, RID_INPUT, nullptr, &byte_count, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) || byte_count == 0) return;
    std::vector<std::uint8_t> buffer(byte_count);
    if (GetRawInputData(input, RID_INPUT, buffer.data(), &byte_count, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return;
    const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
    const std::wstring path = raw_device_name(raw->header.hDevice);
    if (!is_logitech_path(path)) return;
    if (raw->header.dwType == RIM_TYPEHID) {
        const DWORD report_size = raw->data.hid.dwSizeHid;
        for (DWORD index = 0; index < raw->data.hid.dwCount; ++index) {
            print_hid_report(state, path, raw->data.hid.bRawData + index * report_size, report_size);
        }
        return;
    }
    if (raw->header.dwType == RIM_TYPEMOUSE && raw->data.mouse.usButtonFlags != 0) {
        std::wcout << L"[raw mouse] " << path << L" flags=0x" << std::hex << std::setw(4) << std::setfill(L'0')
                   << raw->data.mouse.usButtonFlags << L" data=0x" << std::setw(4) << raw->data.mouse.usButtonData
                   << std::dec << std::setfill(L' ') << L'\n' << std::flush;
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<CaptureState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    } else if (message == WM_INPUT && state != nullptr) {
        print_raw_input(*state, reinterpret_cast<HRAWINPUT>(l_param));
        return 0;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

BOOL WINAPI console_handler(DWORD control_type) {
    if (control_type == CTRL_C_EVENT || control_type == CTRL_BREAK_EVENT || control_type == CTRL_CLOSE_EVENT) {
        if (stop_event != nullptr) SetEvent(stop_event);
        if (capture_thread_id != 0) PostThreadMessageW(capture_thread_id, WM_QUIT, 0, 0);
        return TRUE;
    }
    return FALSE;
}

}

namespace logipro {

int capture_raw_input() {
    const auto infos = enumerate_logitech_hid();
    std::vector<RAWINPUTDEVICE> registrations;
    for (const auto& info : infos) {
        if (info.usage_page == 0 || info.usage == 0) continue;
        bool duplicate = false;
        for (const auto& registration : registrations) {
            if (registration.usUsagePage == info.usage_page && registration.usUsage == info.usage) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) registrations.push_back({info.usage_page, info.usage, RIDEV_INPUTSINK, nullptr});
    }
    if (registrations.empty()) {
        std::wcerr << L"No Logitech HID usages were found for Raw Input registration.\n";
        return 1;
    }

    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) {
        std::wcerr << L"CreateEventW failed: " << GetLastError() << L'\n';
        return 1;
    }
    capture_thread_id = GetCurrentThreadId();
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = window_class_name;
    if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::wcerr << L"RegisterClassW failed: " << GetLastError() << L'\n';
        CloseHandle(stop_event);
        stop_event = nullptr;
        capture_thread_id = 0;
        return 1;
    }
    CaptureState state;
    HWND window = CreateWindowExW(0, window_class_name, L"LogiPro Raw Input", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, &state);
    if (window == nullptr) {
        std::wcerr << L"CreateWindowExW failed: " << GetLastError() << L'\n';
        UnregisterClassW(window_class_name, instance);
        CloseHandle(stop_event);
        stop_event = nullptr;
        capture_thread_id = 0;
        return 1;
    }
    for (auto& registration : registrations) registration.hwndTarget = window;
    if (!RegisterRawInputDevices(registrations.data(), static_cast<UINT>(registrations.size()), sizeof(RAWINPUTDEVICE))) {
        std::wcerr << L"RegisterRawInputDevices failed: " << GetLastError() << L'\n';
        DestroyWindow(window);
        UnregisterClassW(window_class_name, instance);
        CloseHandle(stop_event);
        stop_event = nullptr;
        capture_thread_id = 0;
        return 1;
    }
    if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
        std::wcerr << L"SetConsoleCtrlHandler failed: " << GetLastError() << L'\n';
        for (auto& registration : registrations) {
            registration.dwFlags = RIDEV_REMOVE;
            registration.hwndTarget = nullptr;
        }
        RegisterRawInputDevices(registrations.data(), static_cast<UINT>(registrations.size()), sizeof(RAWINPUTDEVICE));
        DestroyWindow(window);
        UnregisterClassW(window_class_name, instance);
        CloseHandle(stop_event);
        stop_event = nullptr;
        capture_thread_id = 0;
        return 1;
    }

    const bool emergency_hotkey_registered = RegisterHotKey(nullptr, emergency_hotkey_id, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12) != FALSE;
    std::wcout << L"Raw Input capture active for " << registrations.size() << L" HID usages. Press buttons; Ctrl+C or Ctrl+Shift+F12 stops capture.\n";
    if (!emergency_hotkey_registered) std::wcerr << L"Emergency hotkey unavailable: " << GetLastError() << L'\n';
    int result = 0;
    for (;;) {
        const DWORD wait_result = MsgWaitForMultipleObjects(1, &stop_event, FALSE, INFINITE, QS_ALLINPUT);
        if (wait_result == WAIT_OBJECT_0) break;
        if (wait_result == WAIT_OBJECT_0 + 1) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT || (message.message == WM_HOTKEY && message.wParam == emergency_hotkey_id)) {
                    SetEvent(stop_event);
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            continue;
        }
        std::wcerr << L"MsgWaitForMultipleObjects failed: " << GetLastError() << L'\n';
        result = 1;
        break;
    }

    if (emergency_hotkey_registered) UnregisterHotKey(nullptr, emergency_hotkey_id);
    SetConsoleCtrlHandler(console_handler, FALSE);
    for (auto& registration : registrations) {
        registration.dwFlags = RIDEV_REMOVE;
        registration.hwndTarget = nullptr;
    }
    RegisterRawInputDevices(registrations.data(), static_cast<UINT>(registrations.size()), sizeof(RAWINPUTDEVICE));
    DestroyWindow(window);
    UnregisterClassW(window_class_name, instance);
    CloseHandle(stop_event);
    stop_event = nullptr;
    capture_thread_id = 0;
    return result;
}

}

#else

#include <iostream>

namespace logipro {

int capture_raw_input() {
    std::cerr << "Raw Input capture is only implemented on Windows.\n";
    return 1;
}

}

#endif
