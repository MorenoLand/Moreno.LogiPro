#include "logipro/hid_capture.hpp"
#include "logipro/hid.hpp"

#ifdef _WIN32

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct CaptureDevice {
    logipro::HidDeviceInfo info;
    HANDLE handle = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    OVERLAPPED overlapped{};
    std::vector<std::uint8_t> buffer;
    std::vector<std::uint8_t> previous;
};

HANDLE stop_event = nullptr;
DWORD capture_thread_id = 0;

void close_device(CaptureDevice& device) {
    if (device.handle != INVALID_HANDLE_VALUE) {
        CancelIoEx(device.handle, &device.overlapped);
        CloseHandle(device.handle);
        device.handle = INVALID_HANDLE_VALUE;
    }
    if (device.event != nullptr) {
        CloseHandle(device.event);
        device.event = nullptr;
    }
}

void print_report(CaptureDevice& device, DWORD byte_count) {
    if (byte_count == 0) return;
    const auto* bytes = device.buffer.data();
    const bool changed = device.previous.size() != byte_count || std::memcmp(device.previous.data(), bytes, byte_count) != 0;
    if (!changed) return;
    std::wcout << L"[pid=0x" << std::hex << std::setw(4) << std::setfill(L'0') << device.info.product_id
               << L" usage=0x" << std::setw(4) << device.info.usage_page << L":0x" << std::setw(4) << device.info.usage
               << L"] bytes=" << std::dec << byte_count << L":";
    for (DWORD index = 0; index < byte_count; ++index) {
        std::wcout << L' ' << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(bytes[index]);
    }
    std::wcout << std::dec << std::setfill(L' ') << L'\n' << std::flush;
    device.previous.assign(bytes, bytes + byte_count);
}

bool issue_read(CaptureDevice& device) {
    ResetEvent(device.event);
    device.overlapped = {};
    device.overlapped.hEvent = device.event;
    DWORD byte_count = 0;
    if (ReadFile(device.handle, device.buffer.data(), static_cast<DWORD>(device.buffer.size()), &byte_count, &device.overlapped)) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) return true;
    std::wcerr << L"ReadFile failed for " << device.info.path << L": " << error << L'\n';
    return false;
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

int capture_hid_reports() {
    const auto infos = enumerate_logitech_hid();
    std::vector<CaptureDevice> devices;
    devices.reserve(infos.size());
    for (const auto& info : infos) {
        if (info.input_report_length == 0) continue;
        CaptureDevice device;
        device.info = info;
        device.handle = CreateFileW(info.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (device.handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            std::wcerr << L"Cannot open " << info.path << L" for capture: " << error << L'\n';
            continue;
        }
        device.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (device.event == nullptr) {
            const DWORD error = GetLastError();
            std::wcerr << L"CreateEventW failed: " << error << L'\n';
            close_device(device);
            continue;
        }
        device.buffer.resize(info.input_report_length);
        devices.push_back(std::move(device));
        if (!issue_read(devices.back())) {
            close_device(devices.back());
            devices.pop_back();
        }
    }

    if (devices.empty()) {
        std::wcerr << L"No readable Logitech input HID interfaces were found.\n";
        return 1;
    }
    if (devices.size() + 1 > MAXIMUM_WAIT_OBJECTS) {
        std::wcerr << L"Too many HID interfaces for capture.\n";
        for (auto& device : devices) close_device(device);
        return 1;
    }

    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) {
        std::wcerr << L"CreateEventW failed: " << GetLastError() << L'\n';
        for (auto& device : devices) close_device(device);
        return 1;
    }
    capture_thread_id = GetCurrentThreadId();
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
        std::wcerr << L"SetConsoleCtrlHandler failed: " << GetLastError() << L'\n';
        CloseHandle(stop_event);
        stop_event = nullptr;
        capture_thread_id = 0;
        for (auto& device : devices) close_device(device);
        return 1;
    }

    constexpr int emergency_hotkey_id = 2;
    const bool emergency_hotkey_registered = RegisterHotKey(nullptr, emergency_hotkey_id, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12) != FALSE;
    std::vector<HANDLE> wait_handles;
    wait_handles.reserve(devices.size() + 1);
    wait_handles.push_back(stop_event);
    for (const auto& device : devices) wait_handles.push_back(device.event);
    std::wcout << L"Capturing " << devices.size() << L" Logitech HID input interfaces. Press buttons; Ctrl+C or Ctrl+Shift+F12 stops capture.\n";
    if (!emergency_hotkey_registered) std::wcerr << L"Emergency hotkey unavailable: " << GetLastError() << L'\n';

    int result = 0;
    for (;;) {
        const DWORD wait_result = MsgWaitForMultipleObjects(static_cast<DWORD>(wait_handles.size()), wait_handles.data(), FALSE, INFINITE, QS_ALLINPUT);
        if (wait_result == WAIT_OBJECT_0) break;
        if (wait_result > WAIT_OBJECT_0 && wait_result < WAIT_OBJECT_0 + wait_handles.size()) {
            const std::size_t device_index = static_cast<std::size_t>(wait_result - WAIT_OBJECT_0 - 1);
            DWORD byte_count = 0;
            if (!GetOverlappedResult(devices[device_index].handle, &devices[device_index].overlapped, &byte_count, FALSE)) {
                std::wcerr << L"GetOverlappedResult failed: " << GetLastError() << L'\n';
                result = 1;
                break;
            }
            print_report(devices[device_index], byte_count);
            if (!issue_read(devices[device_index])) {
                result = 1;
                break;
            }
            continue;
        }
        if (wait_result == WAIT_OBJECT_0 + wait_handles.size()) {
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
    for (auto& device : devices) close_device(device);
    CloseHandle(stop_event);
    stop_event = nullptr;
    capture_thread_id = 0;
    return result;
}

}

#else

#include <iostream>

namespace logipro {

int capture_hid_reports() {
    std::cerr << "Raw HID capture is only implemented on Windows.\n";
    return 1;
}

}

#endif
