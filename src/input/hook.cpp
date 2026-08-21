#include "logipro/hook.hpp"

#ifdef _WIN32

#include <windows.h>

#include <array>
#include <atomic>
#include <iostream>
#include <optional>

namespace {

HHOOK mouse_hook = nullptr;
DWORD hook_thread_id = 0;
HANDLE stop_event = nullptr;
std::atomic_bool stop_requested = false;
std::array<WORD, 5> binding_keys{};

std::size_t button_index(logipro::MouseButton button) {
    return static_cast<std::size_t>(button);
}

const wchar_t* button_name(logipro::MouseButton button) {
    switch (button) {
    case logipro::MouseButton::Left: return L"Left";
    case logipro::MouseButton::Right: return L"Right";
    case logipro::MouseButton::Middle: return L"Middle";
    case logipro::MouseButton::Back: return L"Back";
    case logipro::MouseButton::Forward: return L"Forward";
    }
    return L"Unknown";
}

const wchar_t* key_name(WORD virtual_key) {
    static wchar_t value[7]{};
    swprintf_s(value, L"0x%02X", virtual_key);
    return value;
}

bool decode_button(WPARAM message, const MSLLHOOKSTRUCT* data, logipro::MouseButton& button, bool& pressed) {
    switch (message) {
    case WM_LBUTTONDOWN: button = logipro::MouseButton::Left; pressed = true; return true;
    case WM_LBUTTONUP: button = logipro::MouseButton::Left; pressed = false; return true;
    case WM_RBUTTONDOWN: button = logipro::MouseButton::Right; pressed = true; return true;
    case WM_RBUTTONUP: button = logipro::MouseButton::Right; pressed = false; return true;
    case WM_MBUTTONDOWN: button = logipro::MouseButton::Middle; pressed = true; return true;
    case WM_MBUTTONUP: button = logipro::MouseButton::Middle; pressed = false; return true;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        const WORD xbutton = HIWORD(data->mouseData);
        if (xbutton == XBUTTON1) button = logipro::MouseButton::Back;
        else if (xbutton == XBUTTON2) button = logipro::MouseButton::Forward;
        else return false;
        pressed = message == WM_XBUTTONDOWN;
        return true;
    }
    default: return false;
    }
}

bool emit_key(WORD virtual_key, bool pressed) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtual_key;
    input.ki.dwFlags = pressed ? 0 : KEYEVENTF_KEYUP;
    return SendInput(1, &input, sizeof(input)) == 1;
}

BOOL WINAPI console_handler(DWORD control_type) {
    if (control_type == CTRL_C_EVENT || control_type == CTRL_BREAK_EVENT || control_type == CTRL_CLOSE_EVENT) {
        stop_requested.store(true, std::memory_order_relaxed);
        if (stop_event != nullptr) SetEvent(stop_event);
        if (hook_thread_id != 0) PostThreadMessageW(hook_thread_id, WM_QUIT, 0, 0);
        return TRUE;
    }
    return FALSE;
}

LRESULT CALLBACK mouse_proc(int code, WPARAM message, LPARAM data_pointer) {
    if (code == HC_ACTION && data_pointer != 0) {
        const auto* data = reinterpret_cast<const MSLLHOOKSTRUCT*>(data_pointer);
        if ((data->flags & LLMHF_INJECTED) == 0) {
            logipro::MouseButton button{};
            bool pressed = false;
            if (decode_button(message, data, button, pressed)) {
                const WORD virtual_key = binding_keys[button_index(button)];
                if (virtual_key != 0) {
                    std::wcout << button_name(button) << (pressed ? L" down" : L" up") << L" -> " << key_name(virtual_key) << L'\n';
                    if (!emit_key(virtual_key, pressed)) {
                        std::wcerr << L"SendInput failed: " << GetLastError() << L'\n';
                    } else {
                        return 1;
                    }
                }
                std::wcout << button_name(button) << (pressed ? L" down\n" : L" up\n");
            }
        }
    }
    return CallNextHookEx(nullptr, code, message, data_pointer);
}

}

namespace logipro {

int run_mouse_bindings(const std::vector<MouseBinding>& bindings) {
    binding_keys = {};
    stop_requested.store(false, std::memory_order_relaxed);
    for (const auto& binding : bindings) {
        binding_keys[static_cast<std::size_t>(binding.button)] = binding.virtual_key;
    }
    hook_thread_id = GetCurrentThreadId();
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) {
        std::wcerr << L"CreateEventW failed: " << GetLastError() << L'\n';
        hook_thread_id = 0;
        return 1;
    }
    mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, GetModuleHandleW(nullptr), 0);
    if (mouse_hook == nullptr) {
        std::wcerr << L"SetWindowsHookExW failed: " << GetLastError() << L'\n';
        CloseHandle(stop_event);
        stop_event = nullptr;
        hook_thread_id = 0;
        return 1;
    }

    if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
        std::wcerr << L"SetConsoleCtrlHandler failed: " << GetLastError() << L'\n';
        UnhookWindowsHookEx(mouse_hook);
        mouse_hook = nullptr;
        CloseHandle(stop_event);
        stop_event = nullptr;
        hook_thread_id = 0;
        return 1;
    }
    constexpr int emergency_hotkey_id = 1;
    const bool emergency_hotkey_registered = RegisterHotKey(nullptr, emergency_hotkey_id, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12) != FALSE;
    std::wcout << (bindings.empty() ? L"Watching mouse buttons." : L"Active mouse bindings.") << L" Press Ctrl+C or Ctrl+Shift+F12 to stop.\n";
    if (binding_keys[button_index(MouseButton::Left)] != 0 || binding_keys[button_index(MouseButton::Right)] != 0) std::wcout << L"Warning: bound primary clicks are suppressed while this process runs.\n";
    int result = 0;
    for (;;) {
        const DWORD wait_result = MsgWaitForMultipleObjects(1, &stop_event, FALSE, INFINITE, QS_ALLINPUT);
        if (wait_result == WAIT_OBJECT_0) break;
        if (wait_result == WAIT_OBJECT_0 + 1) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT || (message.message == WM_HOTKEY && message.wParam == emergency_hotkey_id)) {
                    stop_requested.store(true, std::memory_order_relaxed);
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (stop_requested.load(std::memory_order_relaxed)) break;
            continue;
        }
        std::wcerr << L"MsgWaitForMultipleObjects failed: " << GetLastError() << L'\n';
        result = 1;
        break;
    }

    if (emergency_hotkey_registered) UnregisterHotKey(nullptr, emergency_hotkey_id);
    SetConsoleCtrlHandler(console_handler, FALSE);
    UnhookWindowsHookEx(mouse_hook);
    mouse_hook = nullptr;
    CloseHandle(stop_event);
    stop_event = nullptr;
    hook_thread_id = 0;
    return result;
}

int watch_mouse_buttons() {
    return run_mouse_bindings({});
}

}

#else

#include <iostream>

namespace logipro {

int run_mouse_bindings(const std::vector<MouseBinding>&) {
    std::cerr << "Mouse bindings are only implemented on Windows.\n";
    return 1;
}

int watch_mouse_buttons() {
    return run_mouse_bindings({});
}

}

#endif
