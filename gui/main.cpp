#include "logipro/api.h"
#include "logipro/app.hpp"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

struct UiState {
    GtkLabel* status = nullptr;
    GtkLabel* device_name = nullptr;
    GtkLabel* connection = nullptr;
    GtkLabel* protocol = nullptr;
    GtkLabel* features = nullptr;
    GtkLabel* battery = nullptr;
    GtkLabel* profile_state = nullptr;
    GtkLabel* profile_detail = nullptr;
    GtkLabel* profile_crc = nullptr;
    GtkLabel* profile_lighting = nullptr;
    GtkLabel* lighting_state = nullptr;
    GtkLabel* lighting_zones = nullptr;
    GtkLabel* lighting_control = nullptr;
    GtkGrid* buttons_grid = nullptr;
    GtkWindow* window = nullptr;
    GtkButton* refresh = nullptr;
    GtkButton* lighting_off = nullptr;
    bool busy = false;
    bool lighting_available = false;
};

struct SnapshotResult {
    int status = LOGIPRO_INTERNAL_ERROR;
    logipro_snapshot_t* snapshot = nullptr;
};

bool has_flag(int argc, char* const argv[], std::string_view flag) {
    for (int index = 1; index < argc; ++index) if (std::string_view(argv[index]) == flag) return true;
    return false;
}

GtkWidget* label(const char* text, const char* css_class = nullptr) {
    GtkWidget* widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0.0f);
    if (css_class != nullptr) gtk_widget_add_css_class(widget, css_class);
    return widget;
}

void set_text(GtkLabel* target, const std::string& text) {
    gtk_label_set_text(target, text.c_str());
}

void set_indicator(GtkLabel* target, const char* text, bool good) {
    gtk_widget_remove_css_class(GTK_WIDGET(target), "good");
    gtk_widget_remove_css_class(GTK_WIDGET(target), "warning");
    gtk_widget_add_css_class(GTK_WIDGET(target), good ? "good" : "warning");
    gtk_label_set_text(target, text);
}

void set_status(UiState* state, const char* text, const char* css_class) {
    gtk_widget_remove_css_class(GTK_WIDGET(state->status), "status-neutral");
    gtk_widget_remove_css_class(GTK_WIDGET(state->status), "status-good");
    gtk_widget_remove_css_class(GTK_WIDGET(state->status), "status-warning");
    gtk_widget_remove_css_class(GTK_WIDGET(state->status), "status-error");
    gtk_widget_add_css_class(GTK_WIDGET(state->status), css_class);
    gtk_label_set_text(state->status, text);
}

void set_busy(UiState* state, bool busy) {
    state->busy = busy;
    gtk_widget_set_sensitive(GTK_WIDGET(state->refresh), !busy);
    gtk_widget_set_sensitive(GTK_WIDGET(state->lighting_off), !busy && state->lighting_available);
}

GtkWidget* card(const char* title, const char* caption, GtkWidget* content) {
    GtkWidget* frame = gtk_frame_new(nullptr);
    gtk_widget_add_css_class(frame, "card");
    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(body, "card-content");
    gtk_box_append(GTK_BOX(body), label(title, "section-title"));
    if (caption != nullptr && caption[0] != '\0') gtk_box_append(GTK_BOX(body), label(caption, "section-caption"));
    gtk_box_append(GTK_BOX(body), content);
    gtk_frame_set_child(GTK_FRAME(frame), body);
    return frame;
}

GtkLabel* info_row(GtkGrid* grid, int row, const char* name) {
    GtkWidget* key = label(name, "muted");
    GtkWidget* value = label("—", "value");
    gtk_widget_set_hexpand(value, TRUE);
    gtk_grid_attach(grid, key, 0, row, 1, 1);
    gtk_grid_attach(grid, value, 1, row, 1, 1);
    return GTK_LABEL(value);
}

std::string hex_value(unsigned int value, int width) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setw(width) << std::setfill('0') << value;
    return output.str();
}

std::string hid_key_name(std::uint8_t key) {
    if (key >= 0x04 && key <= 0x1d) return std::string(1, static_cast<char>('A' + key - 0x04));
    if (key >= 0x1e && key <= 0x26) return std::to_string(key - 0x1e + 1);
    if (key == 0x27) return "0";
    if (key >= 0x3a && key <= 0x45) return "F" + std::to_string(key - 0x39);
    if (key >= 0x68 && key <= 0x73) return "F" + std::to_string(key - 0x68 + 13);
    if (key == 0x28) return "Enter";
    if (key == 0x29) return "Escape";
    if (key == 0x2a) return "Backspace";
    if (key == 0x2b) return "Tab";
    if (key == 0x2c) return "Space";
    return "HID " + hex_value(key, 2);
}

std::string mouse_binding_name(std::uint16_t mask) {
    if (mask == 0x0001) return "Left click";
    if (mask == 0x0002) return "Right click";
    if (mask == 0x0004) return "Middle click";
    if (mask == 0x0008) return "Back";
    if (mask == 0x0010) return "Forward";
    return "Mouse " + hex_value(mask, 4);
}

std::string binding_name(const std::array<std::uint8_t, 4>& spec) {
    if (spec[0] == 0x80 && spec[1] == 0x01) return mouse_binding_name(static_cast<std::uint16_t>((spec[2] << 8) | spec[3]));
    if (spec[0] == 0x80 && spec[1] == 0x02) return "Keyboard " + hid_key_name(spec[3]);
    if (spec[0] == 0x90 && spec[1] == 0x05) return "DPI cycle";
    return "Raw " + hex_value(spec[0], 2) + ' ' + hex_value(spec[1], 2) + ' ' + hex_value(spec[2], 2) + ' ' + hex_value(spec[3], 2);
}

GtkWidget* binding_tile(std::uint8_t button, const std::string& name) {
    GtkWidget* tile = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(tile, "binding-tile");
    gtk_widget_set_hexpand(tile, TRUE);
    GtkWidget* number = label(("BUTTON " + std::to_string(button)).c_str(), "binding-number");
    GtkWidget* value = label(name.c_str(), "binding-value");
    gtk_widget_set_hexpand(value, TRUE);
    gtk_box_append(GTK_BOX(tile), number);
    gtk_box_append(GTK_BOX(tile), value);
    return tile;
}

void clear_buttons(UiState* state) {
    GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(state->buttons_grid));
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_grid_remove(state->buttons_grid, child);
        child = next;
    }
}

void reset_ui(UiState* state) {
    set_text(state->device_name, "No compatible Logitech device");
    set_text(state->connection, "Not detected");
    set_text(state->protocol, "—");
    set_text(state->features, "—");
    set_text(state->battery, "Unavailable");
    set_text(state->profile_state, "Unavailable");
    set_text(state->profile_detail, "Connect the receiver to read its onboard profile.");
    set_indicator(state->profile_crc, "Not checked", false);
    set_text(state->profile_lighting, "—");
    set_text(state->lighting_state, "Unavailable");
    set_text(state->lighting_zones, "—");
    set_text(state->lighting_control, "—");
    state->lighting_available = false;
    gtk_widget_set_sensitive(GTK_WIDGET(state->lighting_off), FALSE);
    clear_buttons(state);
    GtkWidget* empty = label("No onboard button map is available.", "muted");
    gtk_widget_set_margin_top(empty, 4);
    gtk_grid_attach(state->buttons_grid, empty, 0, 0, 2, 1);
}

std::string battery_text(const logipro_device_info_t& device) {
    if (!device.battery_readable) return "Unavailable";
    std::string result;
    if (device.battery_percentage_readable) {
        result = (device.battery_percentage_estimated ? "~" : "") + std::to_string(device.battery_percentage) + "%";
        if (device.battery_percentage_estimated) result += " estimated";
    }
    if (device.battery_voltage_readable) {
        if (!result.empty()) result += " • ";
        result += std::to_string(device.battery_voltage_mv) + " mV";
    }
    if (device.battery_feature_id == 0x1001) {
        result += (result.empty() ? "" : " • ") + std::string((device.battery_flags & 0x80) != 0 ? "External power" : "Battery power");
    }
    return result.empty() ? "Available" : result;
}

void apply_snapshot(UiState* state, const SnapshotResult& result) {
    const int status = result.status;
    if (status != LOGIPRO_OK || result.snapshot == nullptr || logipro_snapshot_device_count(result.snapshot) == 0) {
        reset_ui(state);
        set_busy(state, false);
        set_status(state, status == LOGIPRO_OK ? "No device found" : "Unable to read device", status == LOGIPRO_OK ? "status-warning" : "status-error");
        return;
    }
    logipro_device_info_t device{};
    if (logipro_snapshot_get_device(result.snapshot, 0, &device) != LOGIPRO_OK) {
        reset_ui(state);
        set_busy(state, false);
        set_status(state, "Unable to read device", "status-error");
        return;
    }
    set_text(state->device_name, device.product == nullptr || device.product[0] == '\0' ? "Logitech HID++ device" : device.product);
    set_text(state->connection, "USB receiver connected");
    set_text(state->protocol, "HID++ " + std::to_string(device.protocol_major) + "." + std::to_string(device.protocol_minor));
    set_text(state->features, std::to_string(device.feature_count) + " discovered");
    set_text(state->battery, battery_text(device));
    if (device.onboard_profiles_readable) {
        set_indicator(state->profile_state, "Ready", true);
        set_text(state->profile_detail, "Format " + std::to_string(device.profile_format) + " • " + std::to_string(device.button_count) + " buttons • sector " + hex_value(device.active_sector, 4));
        set_indicator(state->profile_crc, device.active_profile_readable && device.active_profile_crc_valid ? "CRC verified" : "CRC unavailable", device.active_profile_readable && device.active_profile_crc_valid);
        set_text(state->profile_lighting, std::to_string(device.active_lighting_count) + " profile lighting record" + (device.active_lighting_count == 1 ? "" : "s"));
    } else {
        set_indicator(state->profile_state, "Unavailable", false);
        set_text(state->profile_detail, "This device does not expose an onboard profile.");
        set_indicator(state->profile_crc, "Not checked", false);
        set_text(state->profile_lighting, "—");
    }
    if (device.lighting_readable) {
        set_indicator(state->lighting_state, "Available", true);
        set_text(state->lighting_zones, std::to_string(device.lighting_declared_zone_count) + " zone" + (device.lighting_declared_zone_count == 1 ? "" : "s"));
        set_text(state->lighting_control, device.lighting_software_control_readable ? (device.lighting_software_control == 0 ? "Firmware" : "Software") : "Unknown");
    } else {
        set_indicator(state->lighting_state, "Unavailable", false);
        set_text(state->lighting_zones, "—");
        set_text(state->lighting_control, "—");
    }
    state->lighting_available = device.onboard_profiles_readable != 0;
    clear_buttons(state);
    const std::uint8_t button_count = device.button_count > 8 ? 8 : device.button_count;
    if (button_count == 0) {
        GtkWidget* empty = label("No onboard button map is available.", "muted");
        gtk_grid_attach(state->buttons_grid, empty, 0, 0, 2, 1);
    } else {
        for (std::uint8_t button = 1; button <= button_count; ++button) {
            std::array<std::uint8_t, 4> spec{};
            const std::string value = logipro_snapshot_get_button(result.snapshot, 0, button, spec.data()) == LOGIPRO_OK ? binding_name(spec) : "Unavailable";
            GtkWidget* tile = binding_tile(button, value);
            gtk_grid_attach(state->buttons_grid, tile, (button - 1) % 2, (button - 1) / 2, 1, 1);
        }
    }
    set_busy(state, false);
    set_status(state, "Connected", "status-good");
}

void destroy_snapshot_result(gpointer data) {
    auto* result = static_cast<SnapshotResult*>(data);
    logipro_snapshot_destroy(result->snapshot);
    delete result;
}

void snapshot_complete(GObject* source, GAsyncResult* async_result, gpointer) {
    auto* state = static_cast<UiState*>(g_object_get_data(source, "logipro-state"));
    if (state == nullptr) return;
    auto* result = static_cast<SnapshotResult*>(g_task_propagate_pointer(G_TASK(async_result), nullptr));
    if (result == nullptr) {
        reset_ui(state);
        set_busy(state, false);
        set_status(state, "Unable to read device", "status-error");
        return;
    }
    apply_snapshot(state, *result);
}

void start_snapshot_read(UiState* state) {
    set_busy(state, true);
    set_status(state, "Reading device", "status-neutral");
    GTask* task = g_task_new(G_OBJECT(state->window), nullptr, snapshot_complete, nullptr);
    g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer, GCancellable*) {
        auto* result = new SnapshotResult();
        result->status = logipro_snapshot_create(&result->snapshot);
        g_task_return_pointer(task, result, destroy_snapshot_result);
    });
    g_object_unref(task);
}

void lighting_complete(GObject* source, GAsyncResult* async_result, gpointer) {
    auto* state = static_cast<UiState*>(g_object_get_data(source, "logipro-state"));
    if (state == nullptr) return;
    const int result = g_task_propagate_int(G_TASK(async_result), nullptr);
    if (result != LOGIPRO_OK) {
        set_busy(state, false);
        set_status(state, "Lighting update failed", "status-error");
        return;
    }
    start_snapshot_read(state);
}

void refresh_clicked(GtkButton*, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    if (!state->busy) start_snapshot_read(state);
}

void lighting_off_clicked(GtkButton*, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    if (state->busy) return;
    set_busy(state, true);
    set_status(state, "Updating lighting", "status-neutral");
    GTask* task = g_task_new(G_OBJECT(state->window), nullptr, lighting_complete, nullptr);
    g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer, GCancellable*) {
        g_task_return_int(task, logipro_profile_lighting_off());
    });
    g_object_unref(task);
}

void install_css() {
    static constexpr const char css[] =
        "window { background: #f4f6fa; color: #1d2635; }"
        ".page { padding: 26px; }"
        ".app-title { font-size: 20px; font-weight: 700; }"
        ".app-subtitle { color: #7a8496; font-size: 12px; }"
        ".section-title { font-size: 15px; font-weight: 700; }"
        ".section-caption { color: #7a8496; font-size: 12px; }"
        ".card { background: #ffffff; border: 1px solid #e2e7ef; border-radius: 14px; }"
        ".card > border { border-radius: 14px; }"
        ".card-content { padding: 18px; }"
        ".muted { color: #7a8496; }"
        ".value { font-weight: 600; }"
        ".status-neutral, .status-good, .status-warning, .status-error { padding: 7px 11px; border-radius: 999px; font-weight: 600; }"
        ".status-neutral { background: #e9edf4; color: #536176; }"
        ".status-good { background: #e3f5e9; color: #237747; }"
        ".status-warning { background: #fff2d8; color: #9a6800; }"
        ".status-error { background: #fde7e7; color: #a23c3c; }"
        ".good { color: #237747; }"
        ".warning { color: #9a6800; }"
        ".binding-tile { background: #f8f9fc; border: 1px solid #edf0f5; border-radius: 10px; padding: 12px; }"
        ".binding-number { color: #7a8496; font-size: 11px; font-weight: 700; }"
        ".binding-value { font-weight: 600; }";
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

void activate(GtkApplication* application, gpointer) {
    install_css();
    GtkWindow* window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_title(window, "LogiPro");
    gtk_window_set_default_size(window, 860, 720);
    GtkWidget* header = gtk_header_bar_new();
    GtkWidget* heading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(heading, "header-heading");
    gtk_box_append(GTK_BOX(heading), label("LogiPro", "app-title"));
    gtk_box_append(GTK_BOX(heading), label("Logitech device control", "app-subtitle"));
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), heading);
    GtkWidget* refresh = gtk_button_new_with_label("Refresh");
    gtk_widget_add_css_class(refresh, "suggested-action");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), refresh);
    gtk_window_set_titlebar(window, header);

    auto* state = new UiState();
    g_object_set_data_full(G_OBJECT(window), "logipro-state", state, [](gpointer value) { delete static_cast<UiState*>(value); });
    state->status = GTK_LABEL(label("Reading device", "status-neutral"));
    state->window = window;
    state->refresh = GTK_BUTTON(refresh);

    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_add_css_class(page, "page");
    GtkWidget* status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(status_row), label("Device status", "section-title"));
    gtk_box_append(GTK_BOX(status_row), GTK_WIDGET(state->status));
    gtk_widget_set_hexpand(GTK_WIDGET(state->status), TRUE);
    gtk_box_append(GTK_BOX(page), status_row);

    GtkWidget* device_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(device_grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(device_grid), 24);
    state->device_name = info_row(GTK_GRID(device_grid), 0, "Device");
    state->connection = info_row(GTK_GRID(device_grid), 1, "Connection");
    state->protocol = info_row(GTK_GRID(device_grid), 2, "Protocol");
    state->features = info_row(GTK_GRID(device_grid), 3, "Features");
    state->battery = info_row(GTK_GRID(device_grid), 4, "Battery");

    GtkWidget* profile_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(profile_grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(profile_grid), 24);
    state->profile_state = info_row(GTK_GRID(profile_grid), 0, "Profile");
    state->profile_detail = info_row(GTK_GRID(profile_grid), 1, "Layout");
    state->profile_crc = info_row(GTK_GRID(profile_grid), 2, "Integrity");
    state->profile_lighting = info_row(GTK_GRID(profile_grid), 3, "Lighting data");

    GtkWidget* overview = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(overview), 16);
    gtk_grid_set_column_homogeneous(GTK_GRID(overview), TRUE);
    gtk_grid_attach(GTK_GRID(overview), card("Device", "The connected receiver and HID++ endpoint.", device_grid), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(overview), card("Onboard profile", "The profile stored inside the mouse or receiver.", profile_grid), 1, 0, 1, 1);
    gtk_box_append(GTK_BOX(page), overview);

    GtkWidget* lighting_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget* lighting_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(lighting_grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(lighting_grid), 24);
    state->lighting_state = info_row(GTK_GRID(lighting_grid), 0, "Live lighting");
    state->lighting_zones = info_row(GTK_GRID(lighting_grid), 1, "Zones");
    state->lighting_control = info_row(GTK_GRID(lighting_grid), 2, "Control");
    gtk_box_append(GTK_BOX(lighting_box), lighting_grid);
    state->lighting_off = GTK_BUTTON(gtk_button_new_with_label("Disable onboard lighting"));
    gtk_widget_add_css_class(GTK_WIDGET(state->lighting_off), "destructive-action");
    gtk_widget_set_halign(GTK_WIDGET(state->lighting_off), GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(lighting_box), GTK_WIDGET(state->lighting_off));
    gtk_box_append(GTK_BOX(page), card("Lighting", "Live capabilities are reported separately from profile effects.", lighting_box));

    GtkWidget* buttons_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(buttons_grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(buttons_grid), 10);
    state->buttons_grid = GTK_GRID(buttons_grid);
    gtk_box_append(GTK_BOX(page), card("Onboard button map", "Assignments currently stored in the active profile.", buttons_grid));

    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), page);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_window_set_child(window, scroll);
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_clicked), state);
    g_signal_connect(state->lighting_off, "clicked", G_CALLBACK(lighting_off_clicked), state);
    start_snapshot_read(state);
    gtk_window_present(window);
}

#ifdef _WIN32
void connect_existing_terminal() {
    bool connected = GetConsoleWindow() != nullptr;
    if (!connected) {
        connected = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE || GetLastError() == ERROR_ACCESS_DENIED;
    }
    if (!connected) return;
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
    std::ios::sync_with_stdio(true);
}
#endif

}

int logipro_app_main(int argc, char** argv) {
    const bool debug = has_flag(argc, argv, "--debug");
    const bool cli = logipro::cli_requested(argc, argv);
#ifdef _WIN32
    if (cli || debug) connect_existing_terminal();
#endif
    logipro_debug_set_enabled(debug ? 1 : 0);
    if (cli) return logipro::run_cli(argc, argv);
    GtkApplication* application = gtk_application_new("com.morenoland.logipro", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), nullptr);
    char app_name[] = "logipro";
    char* app_argv[] = {app_name, nullptr};
    const int status = g_application_run(G_APPLICATION(application), 1, app_argv);
    g_object_unref(application);
    return status;
}
