#include "tray.hpp"

#ifdef _WIN32

#include <windows.h>
#include <shellapi.h>

namespace {

constexpr UINT tray_message = WM_APP + 1;
constexpr UINT tray_open = 1001;
constexpr UINT tray_exit = 1002;
constexpr wchar_t tray_class_name[] = L"LogiProTrayWindow";

struct TrayState {
    HWND window = nullptr;
    GtkWindow* gtk_window = nullptr;
    GApplication* application = nullptr;
    NOTIFYICONDATAW icon{};
};

TrayState* tray_state = nullptr;

void show_menu(HWND window) {
    POINT point{};
    GetCursorPos(&point);
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING, tray_open, L"Open LogiPro");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, tray_exit, L"Exit");
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);
}

void show_window() {
    if (tray_state == nullptr || tray_state->gtk_window == nullptr) return;
    gtk_window_present(tray_state->gtk_window);
}

LRESULT CALLBACK tray_window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == tray_message) {
        if (l_param == WM_LBUTTONUP || l_param == WM_LBUTTONDBLCLK) show_window();
        if (l_param == WM_RBUTTONUP) show_menu(window);
        return 0;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(w_param)) {
        case tray_open:
            show_window();
            return 0;
        case tray_exit:
            if (tray_state != nullptr && tray_state->application != nullptr) g_application_quit(tray_state->application);
            return 0;
        default:
            break;
        }
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

}

bool logipro_tray_start(GtkWindow* window, GApplication* application) {
    if (tray_state != nullptr) return true;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.lpfnWndProc = tray_window_proc;
    window_class.lpszClassName = tray_class_name;
    window_class.hIcon = static_cast<HICON>(LoadIconW(instance, MAKEINTRESOURCEW(101)));
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    auto* state = new TrayState();
    state->gtk_window = window;
    state->application = application;
    state->window = CreateWindowExW(0, tray_class_name, L"LogiPro", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (state->window == nullptr) {
        delete state;
        return false;
    }
    state->icon.cbSize = sizeof(state->icon);
    state->icon.hWnd = state->window;
    state->icon.uID = 1;
    state->icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    state->icon.uCallbackMessage = tray_message;
    state->icon.hIcon = window_class.hIcon == nullptr ? LoadIconW(nullptr, MAKEINTRESOURCEW(32512)) : window_class.hIcon;
    lstrcpyW(state->icon.szTip, L"LogiPro");
    tray_state = state;
    if (Shell_NotifyIconW(NIM_ADD, &state->icon) == FALSE) {
        DestroyWindow(state->window);
        delete state;
        tray_state = nullptr;
        return false;
    }
    return true;
}

void logipro_tray_stop() {
    if (tray_state == nullptr) return;
    Shell_NotifyIconW(NIM_DELETE, &tray_state->icon);
    if (tray_state->window != nullptr) DestroyWindow(tray_state->window);
    delete tray_state;
    tray_state = nullptr;
    UnregisterClassW(tray_class_name, GetModuleHandleW(nullptr));
}

bool logipro_tray_available() {
    return tray_state != nullptr;
}

gboolean logipro_tray_close_request(GtkWindow* window, gpointer) {
    if (!logipro_tray_available()) return FALSE;
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    return TRUE;
}

#else

bool logipro_tray_start(GtkWindow*, GApplication*) { return false; }
void logipro_tray_stop() {}
bool logipro_tray_available() { return false; }
gboolean logipro_tray_close_request(GtkWindow*, gpointer) { return FALSE; }

#endif
