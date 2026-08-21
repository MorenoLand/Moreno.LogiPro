#include "logipro/hid.hpp"
#include "logipro/hidpp.hpp"

#include <windows.h>

#include <iomanip>
#include <sstream>
#include <string>

namespace {

constexpr int refresh_id = 1001;
constexpr int lighting_off_id = 1002;
HWND output_window = nullptr;

std::wstring probe_text() {
    const auto devices = logipro::probe_logitech_hidpp(logipro::enumerate_logitech_hid());
    std::wostringstream output;
    output << L"HID++ devices: " << devices.size() << L"\r\n\r\n";
    for (const auto& device : devices) {
        output << (device.product.empty() ? L"(unnamed)" : device.product)
               << L"  index=0x" << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(device.device_index)
               << L"  protocol=" << std::dec << static_cast<unsigned>(device.protocol_major) << L'.' << static_cast<unsigned>(device.protocol_minor) << L"\r\n";
        output << L"Features: " << device.features.size() << L"\r\n";
        if (device.onboard_profiles.readable) {
            output << L"Onboard profiles: format " << static_cast<unsigned>(device.onboard_profiles.profile_format)
                   << L", buttons " << static_cast<unsigned>(device.onboard_profiles.button_count)
                   << L", active sector " << device.onboard_profiles.active_sector
                   << (device.onboard_profiles.active_profile_crc_valid ? L" (CRC OK)" : L" (CRC BAD)") << L"\r\n";
            for (std::size_t index = 0; index < device.onboard_profiles.active_lighting.size(); ++index) {
                output << L"  Lighting " << index + 1 << L": profile effect 0x" << std::hex << std::setw(2) << std::setfill(L'0')
                       << static_cast<unsigned>(device.onboard_profiles.active_lighting[index][0]) << std::dec << L"\r\n";
            }
        }
        if (device.lighting.readable) {
            output << L"Live lighting: " << static_cast<unsigned>(device.lighting.zone_count) << L" zones, "
                   << (device.lighting.software_control == 0 ? L"firmware control" : L"software control") << L"\r\n";
        }
        output << L"\r\nPath: " << device.path << L"\r\n";
    }
    if (devices.empty()) output << L"No compatible Logitech HID++ device was found.\r\n";
    return output.str();
}

void refresh_output() {
    if (output_window == nullptr) return;
    const auto text = probe_text();
    SetWindowTextW(output_window, text.c_str());
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_CREATE:
        CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 10, 100, 28, window, reinterpret_cast<HMENU>(refresh_id), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Disable onboard lighting", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 120, 10, 190, 28, window, reinterpret_cast<HMENU>(lighting_off_id), nullptr, nullptr);
        output_window = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 10, 48, 760, 480, window, nullptr, nullptr, nullptr);
        SendMessageW(output_window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        refresh_output();
        return 0;
    case WM_SIZE:
        if (output_window != nullptr) MoveWindow(output_window, 10, 48, LOWORD(l_param) - 20, HIWORD(l_param) - 58, TRUE);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w_param) == refresh_id) refresh_output();
        if (LOWORD(w_param) == lighting_off_id) {
            const int result = logipro::disable_onboard_lighting();
            if (result != 0) MessageBoxW(window, L"The onboard lighting write failed. Check the CLI output or device connection.", L"LogiPro", MB_OK | MB_ICONERROR);
            else refresh_output();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
}

}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
    const wchar_t class_name[] = L"LogiProWindow";
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = class_name;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, class_name, L"LogiPro", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 820, 600, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 1;
    ShowWindow(window, show_command);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
