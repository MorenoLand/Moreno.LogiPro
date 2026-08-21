#include "logipro/api.h"
#include "logipro/app.hpp"

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <cairo.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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
    GtkLabel* dpi_current = nullptr;
    GtkLabel* dpi_range = nullptr;
    GtkLabel* dpi_profile_state = nullptr;
    GtkLabel* profile_state = nullptr;
    GtkLabel* profile_detail = nullptr;
    GtkLabel* profile_crc = nullptr;
    GtkLabel* profile_lighting = nullptr;
    GtkLabel* lighting_state = nullptr;
    GtkLabel* lighting_zones = nullptr;
    GtkLabel* lighting_control = nullptr;
    GtkLabel* lighting_battery = nullptr;
    GtkLabel* lighting_effect_hint = nullptr;
    GtkLabel* lighting_rate_value = nullptr;
    GtkLabel* lighting_brightness_value = nullptr;
    GtkGrid* buttons_grid = nullptr;
    GtkWindow* window = nullptr;
    GtkButton* refresh = nullptr;
    GtkButton* lighting_off = nullptr;
    GtkButton* lighting_sync = nullptr;
    GtkDropDown* lighting_effect = nullptr;
    GtkSwitch* lighting_windows = nullptr;
    GtkScale* lighting_rate = nullptr;
    GtkScale* lighting_brightness = nullptr;
    GtkWidget* lighting_controls = nullptr;
    GtkWidget* sensitivity_box = nullptr;
    GtkScale* dpi_live_scale = nullptr;
    GtkLabel* dpi_live_value = nullptr;
    GtkButton* dpi_live_apply = nullptr;
    GtkScale* dpi_profile_scale = nullptr;
    GtkDrawingArea* dpi_rail = nullptr;
    GtkLabel* dpi_selected_value = nullptr;
    GtkLabel* dpi_selected_detail = nullptr;
    GtkButton* dpi_profile_save = nullptr;
    GtkButton* dpi_profile_default = nullptr;
    std::array<GtkButton*, 5> dpi_slots{};
    std::array<std::uint16_t, 5> dpi_values{};
    std::uint8_t dpi_profile_count = 0;
    std::uint8_t dpi_default_index = 0;
    std::uint8_t dpi_shift_index = 0;
    std::size_t dpi_selected_slot = 0;
    std::uint8_t dpi_sensor_index = 0;
    double dpi_min = 50;
    double dpi_max = 25600;
    double dpi_step = 50;
    std::array<std::uint16_t, 16> lighting_effect_ids{};
    std::size_t lighting_effect_count = 0;
    bool busy = false;
    bool lighting_available = false;
    bool lighting_controls_available = false;
    bool dpi_available = false;
};

struct SnapshotResult {
    int status = LOGIPRO_INTERNAL_ERROR;
    logipro_snapshot_t* snapshot = nullptr;
};

enum class DpiOperationKind { Live, Profile, Default };

struct DpiOperation {
    DpiOperationKind kind;
    std::uint8_t sensor = 0;
    std::uint8_t slot = 0;
    std::uint16_t dpi = 0;
};

enum class LightingOperationKind { Effect, SoftwareControl };

struct LightingOperation {
    LightingOperationKind kind;
    std::uint16_t effect_id = 0;
    std::uint16_t period_ms = 8000;
    std::uint8_t brightness = 100;
    bool enabled = false;
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
    gtk_widget_set_sensitive(state->lighting_controls, !busy && state->lighting_controls_available);
    gtk_widget_set_sensitive(state->sensitivity_box, !busy && state->dpi_available);
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

std::string lighting_effect_name(std::uint16_t effect_id) {
    switch (effect_id) {
        case 0: return "Off";
        case 1: return "Fixed";
        case 3: return "Cycle";
        case 4: return "Wave";
        case 5: return "Starlight";
        case 6: return "Light on press";
        case 7: return "Audio visualizer";
        case 8: return "Boot / demo";
        case 10: return "Breathing";
        case 11: return "Ripple";
        case 12: return "Custom";
        default: return "Effect " + hex_value(effect_id, 2);
    }
}

void update_lighting_effect_model(UiState* state, const std::array<std::uint16_t, 16>& ids, std::size_t count, std::size_t selected) {
    if (state->lighting_effect == nullptr) return;
    state->lighting_effect_ids = ids;
    state->lighting_effect_count = std::min(count, state->lighting_effect_ids.size());
    std::vector<std::string> names;
    std::vector<const char*> references;
    if (state->lighting_effect_count == 0) {
        names.emplace_back("Unavailable");
    } else {
        for (std::size_t index = 0; index < state->lighting_effect_count; ++index) names.push_back(lighting_effect_name(state->lighting_effect_ids[index]));
    }
    for (const auto& name : names) references.push_back(name.c_str());
    references.push_back(nullptr);
    GtkStringList* model = gtk_string_list_new(references.data());
    gtk_drop_down_set_model(state->lighting_effect, G_LIST_MODEL(model));
    gtk_drop_down_set_selected(state->lighting_effect, state->lighting_effect_count == 0 ? 0 : static_cast<guint>(std::min(selected, state->lighting_effect_count - 1)));
    g_object_unref(model);
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

std::string dpi_text(std::uint16_t value) {
    return std::to_string(value) + " DPI";
}

std::uint16_t snapped_dpi(UiState* state, double value) {
    const double step = state->dpi_step <= 0 ? 50 : state->dpi_step;
    const double snapped = state->dpi_min + std::round((value - state->dpi_min) / step) * step;
    const double bounded = std::max(state->dpi_min, std::min(state->dpi_max, snapped));
    return static_cast<std::uint16_t>(std::lround(bounded));
}

void update_dpi_controls(UiState* state) {
    for (std::size_t slot = 0; slot < state->dpi_slots.size(); ++slot) {
        GtkButton* button = state->dpi_slots[slot];
        if (button == nullptr) continue;
        const bool active = slot < state->dpi_profile_count;
        gtk_widget_set_visible(GTK_WIDGET(button), active);
        gtk_widget_set_sensitive(GTK_WIDGET(button), active && !state->busy);
        if (!active) continue;
        gtk_button_set_label(button, std::to_string(state->dpi_values[slot]).c_str());
        gtk_widget_remove_css_class(GTK_WIDGET(button), "selected");
        if (slot == state->dpi_selected_slot) gtk_widget_add_css_class(GTK_WIDGET(button), "selected");
    }
    if (state->dpi_profile_count == 0) {
        if (state->dpi_selected_value != nullptr) set_text(state->dpi_selected_value, "Unavailable");
        if (state->dpi_selected_detail != nullptr) set_text(state->dpi_selected_detail, "No onboard DPI levels reported");
        if (state->dpi_profile_save != nullptr) gtk_widget_set_sensitive(GTK_WIDGET(state->dpi_profile_save), FALSE);
        if (state->dpi_profile_default != nullptr) gtk_widget_set_sensitive(GTK_WIDGET(state->dpi_profile_default), FALSE);
    } else {
        if (state->dpi_selected_slot >= state->dpi_profile_count) state->dpi_selected_slot = 0;
        const std::uint16_t value = state->dpi_values[state->dpi_selected_slot];
        if (state->dpi_profile_scale != nullptr) gtk_range_set_value(GTK_RANGE(state->dpi_profile_scale), value);
        if (state->dpi_selected_value != nullptr) set_text(state->dpi_selected_value, dpi_text(value));
        if (state->dpi_selected_detail != nullptr) {
            std::string detail = "Level " + std::to_string(state->dpi_selected_slot + 1);
            if (state->dpi_selected_slot == state->dpi_default_index) detail += " • default";
            if (state->dpi_selected_slot == state->dpi_shift_index) detail += " • DPI shift";
            set_text(state->dpi_selected_detail, detail);
        }
        if (state->dpi_profile_save != nullptr) gtk_widget_set_sensitive(GTK_WIDGET(state->dpi_profile_save), !state->busy);
        if (state->dpi_profile_default != nullptr) gtk_widget_set_sensitive(GTK_WIDGET(state->dpi_profile_default), !state->busy);
    }
    if (state->dpi_rail != nullptr) gtk_widget_queue_draw(GTK_WIDGET(state->dpi_rail));
}

void select_dpi_slot(UiState* state, std::size_t slot) {
    if (state->dpi_profile_count == 0) return;
    state->dpi_selected_slot = slot < state->dpi_profile_count ? slot : 0;
    update_dpi_controls(state);
}

void reset_ui(UiState* state) {
    set_text(state->device_name, "No compatible Logitech device");
    set_text(state->connection, "Not detected");
    set_text(state->protocol, "—");
    set_text(state->features, "—");
    set_text(state->battery, "Unavailable");
    set_text(state->dpi_current, "Unavailable");
    set_text(state->dpi_range, "—");
    set_text(state->dpi_profile_state, "Unavailable");
    state->dpi_profile_count = 0;
    state->dpi_default_index = 0;
    state->dpi_shift_index = 0;
    state->dpi_selected_slot = 0;
    state->dpi_values.fill(0);
    if (state->dpi_live_scale != nullptr) gtk_range_set_value(GTK_RANGE(state->dpi_live_scale), state->dpi_min);
    if (state->dpi_profile_scale != nullptr) gtk_range_set_value(GTK_RANGE(state->dpi_profile_scale), state->dpi_min);
    if (state->dpi_live_value != nullptr) set_text(state->dpi_live_value, "Unavailable");
    update_dpi_controls(state);
    set_text(state->profile_state, "Unavailable");
    set_text(state->profile_detail, "Connect the receiver to read its onboard profile.");
    set_indicator(state->profile_crc, "Not checked", false);
    set_text(state->profile_lighting, "—");
    set_text(state->lighting_state, "Unavailable");
    set_text(state->lighting_zones, "—");
    set_text(state->lighting_control, "—");
    set_text(state->lighting_battery, "Unavailable");
    set_text(state->lighting_effect_hint, "No live lighting effects reported");
    set_text(state->lighting_rate_value, "8000 ms");
    set_text(state->lighting_brightness_value, "100%");
    if (state->lighting_windows != nullptr) gtk_switch_set_active(state->lighting_windows, FALSE);
    if (state->lighting_rate != nullptr) gtk_range_set_value(GTK_RANGE(state->lighting_rate), 8000);
    if (state->lighting_brightness != nullptr) gtk_range_set_value(GTK_RANGE(state->lighting_brightness), 100);
    update_lighting_effect_model(state, {}, 0, 0);
    state->lighting_available = false;
    state->lighting_controls_available = false;
    state->dpi_available = false;
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

void update_dpi_view(UiState* state, const logipro_snapshot_t* snapshot, const logipro_device_info_t& device) {
    logipro_dpi_sensor_info_t sensor{};
    if (!device.dpi_readable || device.dpi_sensor_count == 0 || logipro_snapshot_get_dpi_sensor(snapshot, 0, 0, &sensor) != LOGIPRO_OK) {
        state->dpi_available = false;
        state->dpi_profile_count = 0;
        set_text(state->dpi_current, "Unavailable");
        set_text(state->dpi_range, "This device does not expose adjustable DPI.");
        set_text(state->dpi_profile_state, device.dpi_profile_readable ? "Profile values available" : "Unavailable");
        update_dpi_controls(state);
        return;
    }
    state->dpi_available = true;
    state->dpi_sensor_index = sensor.index;
    set_text(state->dpi_current, std::to_string(sensor.current_dpi) + " DPI");
    if (sensor.min_dpi != 0 && sensor.max_dpi != 0) {
        set_text(state->dpi_range, std::to_string(sensor.min_dpi) + "–" + std::to_string(sensor.max_dpi) + " DPI" + (sensor.step == 0 ? "" : " • " + std::to_string(sensor.step) + " DPI steps"));
    } else {
        set_text(state->dpi_range, "Device range unavailable");
    }
    state->dpi_min = sensor.min_dpi == 0 ? 50 : sensor.min_dpi;
    state->dpi_max = sensor.max_dpi == 0 ? 25600 : sensor.max_dpi;
    state->dpi_step = sensor.step == 0 ? 50 : sensor.step;
    gtk_range_set_range(GTK_RANGE(state->dpi_live_scale), state->dpi_min, state->dpi_max);
    gtk_range_set_increments(GTK_RANGE(state->dpi_live_scale), state->dpi_step, state->dpi_step * 10);
    gtk_range_set_value(GTK_RANGE(state->dpi_live_scale), sensor.current_dpi);
    set_text(state->dpi_live_value, dpi_text(sensor.current_dpi));
    if (device.dpi_profile_readable) {
        set_text(state->dpi_profile_state, std::to_string(device.dpi_profile_count) + " pinned levels • default level " + std::to_string(device.dpi_default_index + 1));
        state->dpi_profile_count = device.dpi_profile_count > state->dpi_values.size() ? static_cast<std::uint8_t>(state->dpi_values.size()) : device.dpi_profile_count;
        state->dpi_default_index = device.dpi_default_index < state->dpi_profile_count ? device.dpi_default_index : 0;
        state->dpi_shift_index = device.dpi_shift_index < state->dpi_profile_count ? device.dpi_shift_index : 0;
        for (std::size_t slot = 0; slot < state->dpi_values.size(); ++slot) state->dpi_values[slot] = device.dpi_profile_values[slot];
        if (state->dpi_selected_slot >= state->dpi_profile_count) state->dpi_selected_slot = state->dpi_default_index;
    } else {
        set_text(state->dpi_profile_state, "No persistent DPI slots available");
        state->dpi_profile_count = 0;
    }
    update_dpi_controls(state);
}

bool lighting_zone_supports(const logipro_snapshot_t* snapshot, std::size_t zone_index, std::uint16_t effect_id) {
    logipro_lighting_zone_info_t zone{};
    if (logipro_snapshot_get_lighting_zone(snapshot, 0, zone_index, &zone) != LOGIPRO_OK || !zone.effect_readable) return false;
    for (std::size_t index = 0; index < zone.effect_count; ++index) {
        std::uint16_t candidate = 0;
        if (logipro_snapshot_get_lighting_effect_id(snapshot, 0, zone_index, index, &candidate) == LOGIPRO_OK && candidate == effect_id) return true;
    }
    return false;
}

void update_lighting_view(UiState* state, const logipro_snapshot_t* snapshot, const logipro_device_info_t& device) {
    set_text(state->lighting_battery, battery_text(device));
    state->lighting_controls_available = false;
    std::array<std::uint16_t, 16> common{};
    std::size_t common_count = 0;
    std::uint16_t current_effect = 0;
    logipro_lighting_zone_info_t primary{};
    if (device.lighting_readable && device.lighting_zone_records > 0 && logipro_snapshot_get_lighting_zone(snapshot, 0, 0, &primary) == LOGIPRO_OK && primary.effect_readable) {
        current_effect = primary.effect;
        for (std::size_t index = 0; index < primary.effect_count && common_count < common.size(); ++index) {
            std::uint16_t effect_id = 0;
            if (logipro_snapshot_get_lighting_effect_id(snapshot, 0, 0, index, &effect_id) != LOGIPRO_OK || std::find(common.begin(), common.begin() + common_count, effect_id) != common.begin() + common_count) continue;
            bool supported = true;
            for (std::size_t zone = 1; zone < device.lighting_zone_records; ++zone) {
                if (!lighting_zone_supports(snapshot, zone, effect_id)) {
                    supported = false;
                    break;
                }
            }
            if (supported) common[common_count++] = effect_id;
        }
        state->lighting_controls_available = device.lighting_software_control_readable && common_count > 0;
        std::size_t selected = 0;
        for (std::size_t index = 0; index < common_count; ++index) if (common[index] == current_effect) selected = index;
        update_lighting_effect_model(state, common, common_count, selected);
        set_text(state->lighting_effect_hint, std::to_string(device.lighting_zone_records) + " zones • " + lighting_effect_name(current_effect));
        unsigned int period = 8000;
        unsigned int brightness = 100;
        if (current_effect == 3 || current_effect == 10 || current_effect == 11) {
            period = (static_cast<unsigned int>(primary.effect_parameters[5]) << 8) | primary.effect_parameters[6];
            brightness = primary.effect_parameters[7] == 0 ? 100 : primary.effect_parameters[7];
        }
        period = std::clamp(period, 100u, 60000u);
        brightness = std::min(brightness, 100u);
        gtk_range_set_value(GTK_RANGE(state->lighting_rate), period);
        gtk_range_set_value(GTK_RANGE(state->lighting_brightness), brightness);
        set_text(state->lighting_rate_value, std::to_string(period) + " ms");
        set_text(state->lighting_brightness_value, std::to_string(brightness) + "%");
    } else {
        update_lighting_effect_model(state, common, 0, 0);
        set_text(state->lighting_effect_hint, "No live lighting effects reported");
        gtk_range_set_value(GTK_RANGE(state->lighting_rate), 8000);
        gtk_range_set_value(GTK_RANGE(state->lighting_brightness), 100);
        set_text(state->lighting_rate_value, "8000 ms");
        set_text(state->lighting_brightness_value, "100%");
    }
    if (state->lighting_windows != nullptr) gtk_switch_set_active(state->lighting_windows, device.lighting_software_control != 0);
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
    update_dpi_view(state, result.snapshot, device);
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
    update_lighting_view(state, result.snapshot, device);
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

void destroy_lighting_operation(gpointer data) {
    delete static_cast<LightingOperation*>(data);
}

void start_lighting_operation(UiState* state, const LightingOperation& operation, const char* status) {
    if (state->busy) return;
    set_busy(state, true);
    set_status(state, status, "status-neutral");
    GTask* task = g_task_new(G_OBJECT(state->window), nullptr, lighting_complete, nullptr);
    g_task_set_task_data(task, new LightingOperation(operation), destroy_lighting_operation);
    g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer task_data, GCancellable*) {
        const auto& operation = *static_cast<LightingOperation*>(task_data);
        const int result = operation.kind == LightingOperationKind::Effect ? logipro_lighting_set_effect(operation.effect_id, operation.period_ms, operation.brightness) : logipro_lighting_set_software_control(operation.enabled ? 1 : 0);
        g_task_return_int(task, result);
    });
    g_object_unref(task);
}

void lighting_sync_clicked(GtkButton*, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    if (state->lighting_effect_count == 0 || state->lighting_effect == nullptr) return;
    const auto selected = gtk_drop_down_get_selected(state->lighting_effect);
    if (selected >= state->lighting_effect_count) return;
    const auto period = static_cast<std::uint16_t>(std::clamp(std::lround(gtk_range_get_value(GTK_RANGE(state->lighting_rate))), 100l, 60000l));
    const auto brightness = static_cast<std::uint8_t>(std::clamp(std::lround(gtk_range_get_value(GTK_RANGE(state->lighting_brightness))), 0l, 100l));
    start_lighting_operation(state, {LightingOperationKind::Effect, state->lighting_effect_ids[selected], period, brightness, false}, "Updating lighting");
}

gboolean lighting_windows_changed(GtkSwitch*, gboolean active, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    if (!state->busy) start_lighting_operation(state, {LightingOperationKind::SoftwareControl, 0, 8000, 100, active != FALSE}, "Updating lighting control");
    return FALSE;
}

void lighting_rate_changed(GtkRange* range, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    set_text(state->lighting_rate_value, std::to_string(static_cast<unsigned int>(std::lround(gtk_range_get_value(range)))) + " ms");
}

void lighting_brightness_changed(GtkRange* range, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    set_text(state->lighting_brightness_value, std::to_string(static_cast<unsigned int>(std::lround(gtk_range_get_value(range)))) + "%");
}

void destroy_dpi_operation(gpointer data) {
    delete static_cast<DpiOperation*>(data);
}

void dpi_operation_complete(GObject* source, GAsyncResult* async_result, gpointer) {
    auto* state = static_cast<UiState*>(g_object_get_data(source, "logipro-state"));
    if (state == nullptr) return;
    const int result = g_task_propagate_int(G_TASK(async_result), nullptr);
    if (result != LOGIPRO_OK) {
        set_busy(state, false);
        set_status(state, "DPI update failed", "status-error");
        return;
    }
    start_snapshot_read(state);
}

void start_dpi_operation(UiState* state, const DpiOperation& operation, const char* status) {
    if (state->busy) return;
    set_busy(state, true);
    set_status(state, status, "status-neutral");
    GTask* task = g_task_new(G_OBJECT(state->window), nullptr, dpi_operation_complete, nullptr);
    g_task_set_task_data(task, new DpiOperation(operation), destroy_dpi_operation);
    g_task_run_in_thread(task, [](GTask* task, gpointer, gpointer task_data, GCancellable*) {
        const auto& operation = *static_cast<DpiOperation*>(task_data);
        int result = LOGIPRO_INTERNAL_ERROR;
        if (operation.kind == DpiOperationKind::Live) result = logipro_dpi_set(operation.sensor, operation.dpi);
        if (operation.kind == DpiOperationKind::Profile) result = logipro_profile_dpi_set(operation.slot, operation.dpi);
        if (operation.kind == DpiOperationKind::Default) result = logipro_profile_dpi_set_default(operation.slot);
        g_task_return_int(task, result);
    });
    g_object_unref(task);
}

void dpi_live_scale_changed(GtkRange* range, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    const auto value = snapped_dpi(state, gtk_range_get_value(range));
    if (std::abs(gtk_range_get_value(range) - value) > 0.1) {
        gtk_range_set_value(range, value);
        return;
    }
    set_text(state->dpi_live_value, dpi_text(value));
}

void dpi_profile_scale_changed(GtkRange* range, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    if (state->dpi_profile_count == 0) return;
    const auto value = snapped_dpi(state, gtk_range_get_value(range));
    if (std::abs(gtk_range_get_value(range) - value) > 0.1) {
        gtk_range_set_value(range, value);
        return;
    }
    state->dpi_values[state->dpi_selected_slot] = value;
    gtk_button_set_label(state->dpi_slots[state->dpi_selected_slot], std::to_string(value).c_str());
    set_text(state->dpi_selected_value, dpi_text(value));
    gtk_widget_queue_draw(GTK_WIDGET(state->dpi_rail));
}

void dpi_level_clicked(GtkButton* button, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    select_dpi_slot(state, static_cast<std::size_t>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "dpi-slot"))));
}

void dpi_rail_draw(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    const double left = 28;
    const double right = std::max(left, static_cast<double>(width - 28));
    const double y = std::max(26.0, static_cast<double>(height) * 0.47);
    const double range = state->dpi_max > state->dpi_min ? state->dpi_max - state->dpi_min : 1;
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgb(cr, 0.15, 0.17, 0.21);
    cairo_set_line_width(cr, 8);
    cairo_move_to(cr, left, y);
    cairo_line_to(cr, right, y);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, 0.12, 0.62, 0.98);
    cairo_set_line_width(cr, 3);
    if (state->dpi_profile_count > 0) {
        const double first = std::clamp((static_cast<double>(state->dpi_values[0]) - state->dpi_min) / range, 0.0, 1.0);
        cairo_move_to(cr, left, y);
        cairo_line_to(cr, left + first * (right - left), y);
        cairo_stroke(cr);
    }
    cairo_set_font_size(cr, 12);
    cairo_select_font_face(cr, "Segoe UI", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    for (std::size_t slot = 0; slot < state->dpi_profile_count; ++slot) {
        const double position = std::clamp((static_cast<double>(state->dpi_values[slot]) - state->dpi_min) / range, 0.0, 1.0);
        const double x = left + position * (right - left);
        cairo_set_source_rgb(cr, slot == state->dpi_selected_slot ? 0.12 : 0.88, slot == state->dpi_selected_slot ? 0.66 : 0.90, slot == state->dpi_selected_slot ? 0.98 : 0.94);
        cairo_arc(cr, x, y, slot == state->dpi_selected_slot ? 9 : 7, 0, 2 * G_PI);
        cairo_fill(cr);
        if (slot == state->dpi_default_index) {
            cairo_set_source_rgb(cr, 1.0, 0.72, 0.08);
            cairo_move_to(cr, x, y - 18);
            cairo_line_to(cr, x + 6, y - 12);
            cairo_line_to(cr, x, y - 6);
            cairo_line_to(cr, x - 6, y - 12);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
        cairo_set_source_rgb(cr, 0.72, 0.75, 0.80);
        const std::string text = std::to_string(state->dpi_values[slot]);
        cairo_text_extents_t extents{};
        cairo_text_extents(cr, text.c_str(), &extents);
        cairo_move_to(cr, x - extents.width / 2, y + 35);
        cairo_show_text(cr, text.c_str());
    }
    cairo_set_source_rgb(cr, 0.43, 0.46, 0.52);
    cairo_set_font_size(cr, 11);
    cairo_move_to(cr, left, height - 8);
    cairo_show_text(cr, std::to_string(static_cast<int>(state->dpi_min)).c_str());
    const std::string maximum = std::to_string(static_cast<int>(state->dpi_max));
    cairo_text_extents_t extents{};
    cairo_text_extents(cr, maximum.c_str(), &extents);
    cairo_move_to(cr, right - extents.width, height - 8);
    cairo_show_text(cr, maximum.c_str());
}

void dpi_rail_pressed(GtkGestureClick*, int, double x, double, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    if (state->busy || state->dpi_profile_count == 0) return;
    const double width = gtk_widget_get_width(GTK_WIDGET(state->dpi_rail));
    const double left = 28;
    const double right = std::max(left, width - 28);
    const double ratio = std::clamp((x - left) / std::max(1.0, right - left), 0.0, 1.0);
    const double target = state->dpi_min + ratio * (state->dpi_max - state->dpi_min);
    std::size_t selected = 0;
    double distance = std::abs(static_cast<double>(state->dpi_values[0]) - target);
    for (std::size_t slot = 1; slot < state->dpi_profile_count; ++slot) {
        const double candidate = std::abs(static_cast<double>(state->dpi_values[slot]) - target);
        if (candidate < distance) {
            selected = slot;
            distance = candidate;
        }
    }
    select_dpi_slot(state, selected);
}

void dpi_live_clicked(GtkButton*, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    const auto dpi = snapped_dpi(state, gtk_range_get_value(GTK_RANGE(state->dpi_live_scale)));
    start_dpi_operation(state, {DpiOperationKind::Live, state->dpi_sensor_index, 0, dpi}, "Applying live DPI");
}

void dpi_profile_apply_clicked(GtkButton*, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    if (state->dpi_profile_count == 0) return;
    const auto slot = static_cast<std::uint8_t>(state->dpi_selected_slot);
    start_dpi_operation(state, {DpiOperationKind::Profile, 0, slot, state->dpi_values[slot]}, "Saving DPI profile");
}

void dpi_profile_default_clicked(GtkButton*, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    if (state->dpi_profile_count == 0) return;
    start_dpi_operation(state, {DpiOperationKind::Default, 0, static_cast<std::uint8_t>(state->dpi_selected_slot), 0}, "Setting default DPI");
}

void install_css() {
    static constexpr const char css[] =
        "* { font-family: 'Segoe UI', sans-serif; }"
        "window { background: #0b0d10; color: #f3f6fb; }"
        "headerbar { background: #0b0d10; border-bottom: 1px solid #252932; box-shadow: none; min-height: 52px; padding: 0 10px; }"
        ".page { padding: 16px 24px 20px; }"
        ".status-row { background: #151a20; border: 1px solid #252c35; border-radius: 10px; padding: 8px 12px; }"
        ".app-title { color: #f5f7fb; font-size: 19px; font-weight: 800; }"
        ".app-subtitle { color: #747d8d; font-size: 11px; }"
        ".section-title { color: #f0f3f8; font-size: 15px; font-weight: 700; }"
        ".section-caption { color: #7f8898; font-size: 12px; }"
        ".card { background: #191c21; border: 1px solid #2a2e36; border-radius: 14px; }"
        ".card > border { border-radius: 14px; }"
        ".card-content { padding: 14px; }"
        ".muted { color: #818a9a; }"
        ".value { color: #eef2f8; font-weight: 600; }"
        ".status-neutral, .status-good, .status-warning, .status-error { padding: 7px 12px; border-radius: 999px; font-weight: 700; }"
        ".status-neutral { background: #20252d; color: #aeb7c5; }"
        ".status-good { background: #123324; color: #73e5a3; }"
        ".status-warning { background: #3b2b12; color: #ffc85c; }"
        ".status-error { background: #401b21; color: #ff8c99; }"
        ".good { color: #73e5a3; }"
        ".warning { color: #ffc85c; }"
        ".tab-switcher { background: transparent; border: none; border-radius: 0; padding: 0; }"
        ".tab-switcher button { background: transparent; border: none; box-shadow: none; color: #8992a1; border-radius: 5px; padding: 4px 11px; min-height: 26px; }"
        ".tab-switcher button:hover { background: #171c22; color: #f2f6fb; }"
        ".tab-switcher button:checked { background: #27303a; color: #f2f6fb; }"
        "button { background: #252a31; color: #e8edf5; border: 1px solid #343a45; border-radius: 6px; padding: 4px 9px; min-height: 26px; }"
        "button:hover { background: #303640; }"
        "button.suggested-action { background: #168ee2; color: #ffffff; border-color: #32adff; }"
        "button.suggested-action:hover { background: #2aa6f4; }"
        "button.destructive-action { background: #9f2733; color: #ffffff; border-color: #d44958; }"
        "button.destructive-action:hover { background: #bd3341; }"
        ".binding-tile { background: #20242b; border: 1px solid #303640; border-radius: 10px; padding: 12px; }"
        ".binding-number { color: #7f8999; font-size: 10px; font-weight: 800; }"
        ".binding-value { color: #f1f4f8; font-weight: 600; }"
        ".dpi-chip { min-width: 72px; min-height: 34px; background: #20242b; border: 1px solid #343a45; font-size: 14px; font-weight: 700; }"
        ".dpi-chip:hover { background: #2a3039; }"
        ".dpi-chip.selected { background: #148fdf; border-color: #48b9ff; color: #ffffff; }"
        ".dpi-value { color: #f5f7fb; font-size: 23px; font-weight: 800; }"
        ".dpi-caption { color: #7f8999; font-size: 11px; font-weight: 700; letter-spacing: 0.4px; }"
        ".dpi-selected { color: #2eaeff; font-size: 17px; font-weight: 800; }"
        ".dpi-scale trough { background: #2b3038; min-height: 8px; border-radius: 999px; }"
        ".dpi-scale highlight { background: #1599ed; min-height: 8px; border-radius: 999px; }"
        ".dpi-scale slider { background: #f3f7fb; border: 3px solid #1599ed; min-width: 18px; min-height: 18px; }"
        ".lighting-tab { background: transparent; border: none; box-shadow: none; color: #8992a1; padding: 3px 0; min-height: 24px; border-radius: 0; }"
        ".lighting-tab.selected { color: #f2f6fb; border-bottom: 2px solid #2eaeff; }"
        ".lighting-caption { color: #8993a2; font-size: 12px; }"
        ".lighting-value { color: #f5f7fb; font-size: 18px; font-weight: 800; }"
        ".lighting-note { color: #7f8999; font-size: 11px; }"
        ".lighting-dropdown, .lighting-dropdown > button { background: #20252c; color: #f2f6fb; border-color: #3a424e; min-height: 34px; }"
        ".lighting-scale trough { background: #2b3038; min-height: 8px; border-radius: 999px; }"
        ".lighting-scale highlight { background: #1599ed; min-height: 8px; border-radius: 999px; }"
        ".lighting-scale slider { background: #f3f7fb; border: 3px solid #1599ed; min-width: 18px; min-height: 18px; }"
        ".mouse-picture { padding: 6px; }"
        ".hotspot { background: #20252c; border: 1px solid #3a424e; color: #eaf1f8; font-size: 11px; padding: 6px 9px; }"
        ".hotspot:hover { background: #168fdf; border-color: #49baff; }"
        ".hero { background: linear-gradient(135deg, #1a2027, #121519); border: 1px solid #2d3540; border-radius: 14px; padding: 15px 18px; }"
        ".hero-title { color: #f5f7fb; font-size: 23px; font-weight: 800; }"
        ".hero-caption { color: #8993a2; font-size: 12px; }"
        ".hero-stat { color: #2caeff; font-size: 25px; font-weight: 800; }"
        "headerbar button.titlebutton, headerbar windowcontrols button, windowcontrols button { background: transparent; border: none; box-shadow: none; border-radius: 0; color: #8e97a5; min-width: 30px; min-height: 30px; padding: 0; }"
        "headerbar button.titlebutton:hover, headerbar windowcontrols button:hover, windowcontrols button:hover { background: #1d2229; color: #f3f6fb; }"
        "headerbar button.titlebutton:active, headerbar windowcontrols button:active, windowcontrols button:active { background: #27303a; }";
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

GtkWidget* mouse_picture(int width, int height) {
    GtkWidget* picture = gtk_picture_new_for_resource("/com/morenoland/logipro/assets/logipro-mouse.svg");
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_size_request(picture, width, height);
    gtk_widget_add_css_class(picture, "mouse-picture");
    return picture;
}

GtkWidget* mouse_hotspot(GtkWidget* overlay, const char* text, GtkAlign horizontal, GtkAlign vertical, int top, int start, int end) {
    GtkWidget* button = gtk_button_new_with_label(text);
    gtk_widget_add_css_class(button, "hotspot");
    gtk_widget_set_can_focus(button, FALSE);
    gtk_widget_set_halign(button, horizontal);
    gtk_widget_set_valign(button, vertical);
    gtk_widget_set_margin_top(button, top);
    gtk_widget_set_margin_start(button, start);
    gtk_widget_set_margin_end(button, end);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), button);
    return button;
}

void activate(GtkApplication* application, gpointer) {
    install_css();
    GtkWindow* window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_title(window, "LogiPro");
    gtk_window_set_default_size(window, 1080, 720);
    GtkWidget* header = gtk_header_bar_new();
    GtkWidget* heading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(heading), label("LogiPro", "app-title"));
    gtk_box_append(GTK_BOX(heading), label("Open HID++ control for Logitech devices", "app-subtitle"));
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), heading);
    gtk_widget_set_margin_end(heading, 18);
    GtkWidget* title_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title_spacer);
    GtkWidget* refresh = gtk_button_new_with_label("Refresh");
    gtk_widget_add_css_class(refresh, "suggested-action");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), refresh);
    gtk_window_set_titlebar(window, header);

    auto* state = new UiState();
    g_object_set_data_full(G_OBJECT(window), "logipro-state", state, [](gpointer value) { delete static_cast<UiState*>(value); });
    state->status = GTK_LABEL(label("Reading device", "status-neutral"));
    state->window = window;
    state->refresh = GTK_BUTTON(refresh);

    GtkWidget* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 17);
    gtk_widget_add_css_class(page, "page");
    GtkWidget* status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(status_row, "status-row");
    gtk_box_append(GTK_BOX(status_row), label("Device status", "section-title"));
    gtk_box_append(GTK_BOX(status_row), GTK_WIDGET(state->status));
    gtk_widget_set_hexpand(GTK_WIDGET(state->status), TRUE);
    gtk_box_append(GTK_BOX(page), status_row);

    GtkWidget* overview_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 17);
    GtkWidget* hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_add_css_class(hero, "hero");
    GtkWidget* hero_copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_valign(hero_copy, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(hero_copy, TRUE);
    gtk_box_append(GTK_BOX(hero_copy), label("PRO WIRELESS", "hero-title"));
    gtk_box_append(GTK_BOX(hero_copy), label("A lightweight HID++ control surface for your mouse.", "hero-caption"));
    gtk_box_append(GTK_BOX(hero_copy), label("No vendor service required.", "hero-caption"));
    gtk_box_append(GTK_BOX(hero), hero_copy);
    gtk_box_append(GTK_BOX(hero), mouse_picture(190, 235));
    gtk_box_append(GTK_BOX(overview_page), hero);

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
    gtk_grid_set_column_spacing(GTK_GRID(overview), 17);
    gtk_grid_set_column_homogeneous(GTK_GRID(overview), TRUE);
    gtk_grid_attach(GTK_GRID(overview), card("Device", "The connected receiver and HID++ endpoint.", device_grid), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(overview), card("Onboard profile", "The profile stored inside the mouse or receiver.", profile_grid), 1, 0, 1, 1);
    gtk_box_append(GTK_BOX(overview_page), overview);

    GtkWidget* sensitivity_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 17);
    GtkWidget* sensitivity_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 17);
    state->sensitivity_box = sensitivity_box;
    GtkWidget* dpi_info_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(dpi_info_grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(dpi_info_grid), 24);
    state->dpi_current = info_row(GTK_GRID(dpi_info_grid), 0, "Current DPI");
    state->dpi_range = info_row(GTK_GRID(dpi_info_grid), 1, "Sensor range");
    state->dpi_profile_state = info_row(GTK_GRID(dpi_info_grid), 2, "Onboard levels");
    gtk_box_append(GTK_BOX(sensitivity_box), card("Sensitivity (DPI)", "Tune the live sensor and the five values stored in onboard memory.", dpi_info_grid));

    GtkWidget* dpi_columns = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(dpi_columns), 17);
    gtk_grid_set_column_homogeneous(GTK_GRID(dpi_columns), FALSE);

    GtkWidget* levels_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 13);
    gtk_box_append(GTK_BOX(levels_box), label("DPI SPEEDS", "dpi-caption"));
    GtkWidget* level_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(level_grid), 9);
    gtk_grid_set_column_spacing(GTK_GRID(level_grid), 9);
    for (std::size_t slot = 0; slot < state->dpi_slots.size(); ++slot) {
        state->dpi_slots[slot] = GTK_BUTTON(gtk_button_new_with_label("—"));
        gtk_widget_add_css_class(GTK_WIDGET(state->dpi_slots[slot]), "dpi-chip");
        g_object_set_data(G_OBJECT(state->dpi_slots[slot]), "dpi-slot", GINT_TO_POINTER(static_cast<int>(slot)));
        g_signal_connect(state->dpi_slots[slot], "clicked", G_CALLBACK(dpi_level_clicked), state);
        gtk_grid_attach(GTK_GRID(level_grid), GTK_WIDGET(state->dpi_slots[slot]), static_cast<int>(slot % 2), static_cast<int>(slot / 2), 1, 1);
    }
    gtk_box_append(GTK_BOX(levels_box), level_grid);
    gtk_box_append(GTK_BOX(levels_box), label("Click a level, then drag the rail to edit it.", "section-caption"));
    GtkWidget* live_card_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 11);
    GtkWidget* live_heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(live_heading), label("LIVE SENSOR", "dpi-caption"));
    state->dpi_live_value = GTK_LABEL(label("Unavailable", "dpi-value"));
    gtk_widget_set_hexpand(GTK_WIDGET(state->dpi_live_value), TRUE);
    gtk_label_set_xalign(state->dpi_live_value, 1.0f);
    gtk_box_append(GTK_BOX(live_heading), GTK_WIDGET(state->dpi_live_value));
    gtk_box_append(GTK_BOX(live_card_box), live_heading);
    state->dpi_live_scale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 50, 25600, 50));
    gtk_scale_set_draw_value(state->dpi_live_scale, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(state->dpi_live_scale), "dpi-scale");
    gtk_box_append(GTK_BOX(live_card_box), GTK_WIDGET(state->dpi_live_scale));
    GtkWidget* live_limits = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(live_limits), label("100", "muted"));
    GtkWidget* live_limit_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(live_limit_spacer, TRUE);
    gtk_box_append(GTK_BOX(live_limits), live_limit_spacer);
    gtk_box_append(GTK_BOX(live_limits), label("25,600", "muted"));
    gtk_box_append(GTK_BOX(live_card_box), live_limits);
    state->dpi_live_apply = GTK_BUTTON(gtk_button_new_with_label("Apply live DPI"));
    gtk_widget_add_css_class(GTK_WIDGET(state->dpi_live_apply), "suggested-action");
    gtk_widget_set_halign(GTK_WIDGET(state->dpi_live_apply), GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(live_card_box), GTK_WIDGET(state->dpi_live_apply));
    gtk_box_append(GTK_BOX(levels_box), card("Live sensor", "This changes the active sensor value immediately.", live_card_box));
    gtk_grid_attach(GTK_GRID(dpi_columns), card("Pinned levels", "The mouse DPI button cycles these values.", levels_box), 0, 0, 1, 1);

    GtkWidget* rail_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 13);
    GtkWidget* rail_heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_append(GTK_BOX(rail_heading), label("DPI SPEEDS", "dpi-caption"));
    gtk_widget_set_hexpand(rail_heading, TRUE);
    gtk_box_append(GTK_BOX(rail_heading), label("Yellow diamond = default", "section-caption"));
    gtk_box_append(GTK_BOX(rail_box), rail_heading);
    state->dpi_rail = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_widget_set_hexpand(GTK_WIDGET(state->dpi_rail), TRUE);
    gtk_widget_set_size_request(GTK_WIDGET(state->dpi_rail), 440, 100);
    gtk_drawing_area_set_draw_func(state->dpi_rail, dpi_rail_draw, state, nullptr);
    GtkGestureClick* rail_gesture = GTK_GESTURE_CLICK(gtk_gesture_click_new());
    gtk_widget_add_controller(GTK_WIDGET(state->dpi_rail), GTK_EVENT_CONTROLLER(rail_gesture));
    g_signal_connect(rail_gesture, "pressed", G_CALLBACK(dpi_rail_pressed), state);
    gtk_box_append(GTK_BOX(rail_box), GTK_WIDGET(state->dpi_rail));
    state->dpi_profile_scale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 50, 25600, 50));
    gtk_scale_set_draw_value(state->dpi_profile_scale, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(state->dpi_profile_scale), "dpi-scale");
    gtk_box_append(GTK_BOX(rail_box), label("Drag to set the selected level", "section-caption"));
    gtk_box_append(GTK_BOX(rail_box), GTK_WIDGET(state->dpi_profile_scale));
    GtkWidget* selected_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* selected_copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_box_append(GTK_BOX(selected_copy), label("SELECTED LEVEL", "dpi-caption"));
    state->dpi_selected_value = GTK_LABEL(label("Unavailable", "dpi-selected"));
    state->dpi_selected_detail = GTK_LABEL(label("No onboard DPI levels reported", "section-caption"));
    gtk_box_append(GTK_BOX(selected_copy), GTK_WIDGET(state->dpi_selected_value));
    gtk_box_append(GTK_BOX(selected_copy), GTK_WIDGET(state->dpi_selected_detail));
    gtk_widget_set_hexpand(selected_copy, TRUE);
    gtk_box_append(GTK_BOX(selected_row), selected_copy);
    state->dpi_profile_save = GTK_BUTTON(gtk_button_new_with_label("Save level"));
    state->dpi_profile_default = GTK_BUTTON(gtk_button_new_with_label("Make default"));
    gtk_box_append(GTK_BOX(selected_row), GTK_WIDGET(state->dpi_profile_save));
    gtk_box_append(GTK_BOX(selected_row), GTK_WIDGET(state->dpi_profile_default));
    gtk_box_append(GTK_BOX(rail_box), selected_row);
    gtk_grid_attach(GTK_GRID(dpi_columns), card("DPI speeds", "Choose a stored level or place it precisely on the rail.", rail_box), 1, 0, 1, 1);
    gtk_box_append(GTK_BOX(sensitivity_box), dpi_columns);
    gtk_box_append(GTK_BOX(sensitivity_page), sensitivity_box);

    GtkWidget* lighting_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    GtkWidget* lighting_heading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(lighting_heading), label("LIGHTSYNC", "hero-title"));
    gtk_box_append(GTK_BOX(lighting_heading), label("Control the live lighting effect and the settings stored by the device.", "lighting-caption"));
    gtk_box_append(GTK_BOX(lighting_page), lighting_heading);

    GtkWidget* lighting_columns = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(lighting_columns), 17);
    gtk_grid_set_column_homogeneous(GTK_GRID(lighting_columns), TRUE);

    GtkWidget* lighting_controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    state->lighting_controls = lighting_controls;
    GtkWidget* windows_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(windows_row), label("WINDOWS SETTINGS", "dpi-caption"));
    GtkWidget* windows_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(windows_spacer, TRUE);
    gtk_box_append(GTK_BOX(windows_row), windows_spacer);
    state->lighting_windows = GTK_SWITCH(gtk_switch_new());
    gtk_widget_set_valign(GTK_WIDGET(state->lighting_windows), GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(windows_row), GTK_WIDGET(state->lighting_windows));
    gtk_box_append(GTK_BOX(lighting_controls), windows_row);

    GtkWidget* zone_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget* primary_zone = gtk_button_new_with_label("PRIMARY");
    GtkWidget* logo_zone = gtk_button_new_with_label("LOGO");
    gtk_widget_add_css_class(primary_zone, "lighting-tab");
    gtk_widget_add_css_class(primary_zone, "selected");
    gtk_widget_add_css_class(logo_zone, "lighting-tab");
    gtk_widget_set_can_focus(primary_zone, FALSE);
    gtk_widget_set_can_focus(logo_zone, FALSE);
    gtk_box_append(GTK_BOX(zone_row), primary_zone);
    gtk_box_append(GTK_BOX(zone_row), logo_zone);
    gtk_box_append(GTK_BOX(lighting_controls), zone_row);

    GtkWidget* effect_label = label("EFFECT", "dpi-caption");
    gtk_box_append(GTK_BOX(lighting_controls), effect_label);
    state->lighting_effect = GTK_DROP_DOWN(gtk_drop_down_new(nullptr, nullptr));
    gtk_widget_add_css_class(GTK_WIDGET(state->lighting_effect), "lighting-dropdown");
    gtk_widget_set_hexpand(GTK_WIDGET(state->lighting_effect), TRUE);
    gtk_box_append(GTK_BOX(lighting_controls), GTK_WIDGET(state->lighting_effect));
    update_lighting_effect_model(state, {}, 0, 0);
    gtk_box_append(GTK_BOX(lighting_controls), label("Effects and controls are limited to capabilities shared by the lighting zones.", "lighting-note"));

    GtkWidget* rate_heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(rate_heading), label("EFFECT RATE", "dpi-caption"));
    state->lighting_rate_value = GTK_LABEL(label("8000 ms", "value"));
    gtk_widget_set_hexpand(GTK_WIDGET(state->lighting_rate_value), TRUE);
    gtk_label_set_xalign(state->lighting_rate_value, 1.0f);
    gtk_box_append(GTK_BOX(rate_heading), GTK_WIDGET(state->lighting_rate_value));
    gtk_box_append(GTK_BOX(lighting_controls), rate_heading);
    state->lighting_rate = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 100, 60000, 100));
    gtk_scale_set_draw_value(state->lighting_rate, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(state->lighting_rate), "lighting-scale");
    gtk_range_set_value(GTK_RANGE(state->lighting_rate), 8000);
    gtk_box_append(GTK_BOX(lighting_controls), GTK_WIDGET(state->lighting_rate));

    GtkWidget* brightness_heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(brightness_heading), label("EFFECT BRIGHTNESS", "dpi-caption"));
    state->lighting_brightness_value = GTK_LABEL(label("100%", "value"));
    gtk_widget_set_hexpand(GTK_WIDGET(state->lighting_brightness_value), TRUE);
    gtk_label_set_xalign(state->lighting_brightness_value, 1.0f);
    gtk_box_append(GTK_BOX(brightness_heading), GTK_WIDGET(state->lighting_brightness_value));
    gtk_box_append(GTK_BOX(lighting_controls), brightness_heading);
    state->lighting_brightness = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1));
    gtk_scale_set_draw_value(state->lighting_brightness, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(state->lighting_brightness), "lighting-scale");
    gtk_range_set_value(GTK_RANGE(state->lighting_brightness), 100);
    gtk_box_append(GTK_BOX(lighting_controls), GTK_WIDGET(state->lighting_brightness));

    state->lighting_effect_hint = GTK_LABEL(label("No live lighting effects reported", "lighting-note"));
    gtk_box_append(GTK_BOX(lighting_controls), GTK_WIDGET(state->lighting_effect_hint));
    GtkWidget* lighting_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    state->lighting_sync = GTK_BUTTON(gtk_button_new_with_label("Sync lighting zones"));
    gtk_widget_add_css_class(GTK_WIDGET(state->lighting_sync), "suggested-action");
    gtk_box_append(GTK_BOX(lighting_actions), GTK_WIDGET(state->lighting_sync));
    state->lighting_off = GTK_BUTTON(gtk_button_new_with_label("Disable onboard lighting"));
    gtk_widget_add_css_class(GTK_WIDGET(state->lighting_off), "destructive-action");
    gtk_box_append(GTK_BOX(lighting_actions), GTK_WIDGET(state->lighting_off));
    gtk_box_append(GTK_BOX(lighting_controls), lighting_actions);
    gtk_grid_attach(GTK_GRID(lighting_columns), card("Lighting controls", "The same live effect is applied to every compatible zone.", lighting_controls), 0, 0, 1, 1);

    GtkWidget* lighting_visual = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget* battery_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(battery_row), label("BATTERY LEVEL", "dpi-caption"));
    GtkWidget* battery_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(battery_spacer, TRUE);
    gtk_box_append(GTK_BOX(battery_row), battery_spacer);
    state->lighting_battery = GTK_LABEL(label("Unavailable", "lighting-value"));
    gtk_label_set_xalign(state->lighting_battery, 1.0f);
    gtk_box_append(GTK_BOX(battery_row), GTK_WIDGET(state->lighting_battery));
    gtk_box_append(GTK_BOX(lighting_visual), battery_row);
    GtkWidget* lighting_picture = mouse_picture(300, 340);
    gtk_widget_set_vexpand(lighting_picture, TRUE);
    gtk_box_append(GTK_BOX(lighting_visual), lighting_picture);
    GtkWidget* lighting_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(lighting_grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(lighting_grid), 20);
    state->lighting_state = info_row(GTK_GRID(lighting_grid), 0, "Live lighting");
    state->lighting_zones = info_row(GTK_GRID(lighting_grid), 1, "Zones");
    state->lighting_control = info_row(GTK_GRID(lighting_grid), 2, "Control");
    gtk_box_append(GTK_BOX(lighting_visual), card("Device lighting", "Live capabilities reported by HID++.", lighting_grid));
    gtk_grid_attach(GTK_GRID(lighting_columns), card("Lighting preview", "Primary and logo zones are shown together.", lighting_visual), 1, 0, 1, 1);
    gtk_box_append(GTK_BOX(lighting_page), lighting_columns);

    GtkWidget* mapping_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 17);
    GtkWidget* mouse_visual = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(mouse_visual), mouse_picture(340, 430));
    mouse_hotspot(mouse_visual, "Primary", GTK_ALIGN_START, GTK_ALIGN_START, 100, 0, 0);
    mouse_hotspot(mouse_visual, "Secondary", GTK_ALIGN_END, GTK_ALIGN_START, 100, 0, 0);
    mouse_hotspot(mouse_visual, "Middle", GTK_ALIGN_CENTER, GTK_ALIGN_START, 68, 0, 0);
    mouse_hotspot(mouse_visual, "Back", GTK_ALIGN_START, GTK_ALIGN_START, 224, 0, 0);
    mouse_hotspot(mouse_visual, "Forward", GTK_ALIGN_START, GTK_ALIGN_START, 270, 0, 0);
    mouse_hotspot(mouse_visual, "Back", GTK_ALIGN_END, GTK_ALIGN_START, 224, 0, 0);
    mouse_hotspot(mouse_visual, "Forward", GTK_ALIGN_END, GTK_ALIGN_START, 270, 0, 0);
    mouse_hotspot(mouse_visual, "DPI", GTK_ALIGN_CENTER, GTK_ALIGN_START, 350, 0, 0);
    GtkWidget* buttons_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(buttons_grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(buttons_grid), 10);
    state->buttons_grid = GTK_GRID(buttons_grid);
    GtkWidget* mapping_columns = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(mapping_columns), 17);
    gtk_grid_attach(GTK_GRID(mapping_columns), card("Mouse layout", "The physical controls exposed by the active profile.", mouse_visual), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(mapping_columns), card("Onboard assignments", "Assignments currently stored in the active profile.", buttons_grid), 1, 0, 1, 1);
    gtk_box_append(GTK_BOX(mapping_page), mapping_columns);

    GtkWidget* stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_stack_add_titled(GTK_STACK(stack), overview_page, "overview", "Overview");
    gtk_stack_add_titled(GTK_STACK(stack), sensitivity_page, "sensitivity", "Sensitivity");
    gtk_stack_add_titled(GTK_STACK(stack), lighting_page, "lighting", "Lighting");
    gtk_stack_add_titled(GTK_STACK(stack), mapping_page, "mapping", "Onboard Mapping");
    GtkWidget* switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
    gtk_widget_add_css_class(switcher, "tab-switcher");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), switcher);
    gtk_box_append(GTK_BOX(page), stack);

    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), page);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_window_set_child(window, scroll);
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_clicked), state);
    g_signal_connect(state->lighting_off, "clicked", G_CALLBACK(lighting_off_clicked), state);
    g_signal_connect(state->lighting_sync, "clicked", G_CALLBACK(lighting_sync_clicked), state);
    g_signal_connect(state->lighting_windows, "state-set", G_CALLBACK(lighting_windows_changed), state);
    g_signal_connect(state->lighting_rate, "value-changed", G_CALLBACK(lighting_rate_changed), state);
    g_signal_connect(state->lighting_brightness, "value-changed", G_CALLBACK(lighting_brightness_changed), state);
    g_signal_connect(state->dpi_live_apply, "clicked", G_CALLBACK(dpi_live_clicked), state);
    g_signal_connect(state->dpi_live_scale, "value-changed", G_CALLBACK(dpi_live_scale_changed), state);
    g_signal_connect(state->dpi_profile_scale, "value-changed", G_CALLBACK(dpi_profile_scale_changed), state);
    g_signal_connect(state->dpi_profile_save, "clicked", G_CALLBACK(dpi_profile_apply_clicked), state);
    g_signal_connect(state->dpi_profile_default, "clicked", G_CALLBACK(dpi_profile_default_clicked), state);
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
