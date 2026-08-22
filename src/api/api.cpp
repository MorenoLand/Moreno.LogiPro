#include "logipro/api.h"

#include "debug.hpp"
#include "logipro/hid.hpp"
#include "logipro/hid_capture.hpp"
#include "logipro/hidpp.hpp"
#include "logipro/hook.hpp"

#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

struct logipro_snapshot {
    std::vector<logipro::HidDeviceInfo> hid;
    std::vector<logipro::HidppDeviceInfo> devices;
    std::vector<std::string> hid_paths;
    std::vector<std::string> hid_products;
    std::vector<std::string> device_paths;
    std::vector<std::string> device_products;
};

namespace {

std::atomic_bool debug_state{false};
thread_local std::string last_error;

void append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) codepoint = 0xfffd;
    if (codepoint <= 0x7f) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

std::string utf8(const std::wstring& value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        std::uint32_t codepoint = static_cast<std::uint32_t>(value[index]);
#if WCHAR_MAX == 0xffff
        if (codepoint >= 0xd800 && codepoint <= 0xdbff && index + 1 < value.size()) {
            const std::uint32_t low = static_cast<std::uint32_t>(value[index + 1]);
            if (low >= 0xdc00 && low <= 0xdfff) {
                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                ++index;
            }
        }
#endif
        append_utf8(output, codepoint);
    }
    return output;
}

void clear_error() {
    last_error.clear();
}

int fail(int code, const char* message) {
    last_error = message;
    return code;
}

int operation_result(int result) {
    if (result == 0) return LOGIPRO_OK;
    if (last_error.empty()) last_error = "LogiPro operation failed.";
    return LOGIPRO_IO_ERROR;
}

int validate_snapshot(const logipro_snapshot_t* snapshot) {
    return snapshot == nullptr ? fail(LOGIPRO_INVALID_ARGUMENT, "Snapshot is null.") : LOGIPRO_OK;
}

}

namespace logipro {

bool debug_enabled() {
    return debug_state.load(std::memory_order_relaxed);
}

void set_debug_enabled(bool enabled) {
    debug_state.store(enabled, std::memory_order_relaxed);
}

}

extern "C" {

int logipro_snapshot_create(logipro_snapshot_t** out_snapshot) {
    clear_error();
    if (out_snapshot == nullptr) return fail(LOGIPRO_INVALID_ARGUMENT, "Output snapshot is null.");
    *out_snapshot = nullptr;
    try {
        auto snapshot = std::unique_ptr<logipro_snapshot>(new logipro_snapshot());
        snapshot->hid = logipro::enumerate_logitech_hid();
        snapshot->hid_paths.reserve(snapshot->hid.size());
        snapshot->hid_products.reserve(snapshot->hid.size());
        for (const auto& device : snapshot->hid) {
            snapshot->hid_paths.push_back(utf8(device.path));
            snapshot->hid_products.push_back(utf8(device.product));
        }
        snapshot->devices = logipro::probe_logitech_hidpp(snapshot->hid);
        snapshot->device_paths.reserve(snapshot->devices.size());
        snapshot->device_products.reserve(snapshot->devices.size());
        for (const auto& device : snapshot->devices) {
            snapshot->device_paths.push_back(utf8(device.path));
            snapshot->device_products.push_back(utf8(device.product));
        }
        *out_snapshot = snapshot.release();
        return LOGIPRO_OK;
    } catch (const std::exception& error) {
        return fail(LOGIPRO_INTERNAL_ERROR, error.what());
    } catch (...) {
        return fail(LOGIPRO_INTERNAL_ERROR, "Unknown LogiPro snapshot error.");
    }
}

void logipro_snapshot_destroy(logipro_snapshot_t* snapshot) {
    delete snapshot;
}

size_t logipro_snapshot_hid_count(const logipro_snapshot_t* snapshot) {
    return snapshot == nullptr ? 0 : snapshot->hid.size();
}

int logipro_snapshot_get_hid(const logipro_snapshot_t* snapshot, size_t index, logipro_hid_info_t* out_info) {
    clear_error();
    if (validate_snapshot(snapshot) != LOGIPRO_OK || out_info == nullptr) return out_info == nullptr ? fail(LOGIPRO_INVALID_ARGUMENT, "Output HID info is null.") : LOGIPRO_INVALID_ARGUMENT;
    if (index >= snapshot->hid.size()) return fail(LOGIPRO_NOT_FOUND, "HID interface index is out of range.");
    const auto& device = snapshot->hid[index];
    *out_info = {};
    out_info->path = snapshot->hid_paths[index].c_str();
    out_info->product = snapshot->hid_products[index].c_str();
    out_info->vendor_id = device.vendor_id;
    out_info->product_id = device.product_id;
    out_info->usage_page = device.usage_page;
    out_info->usage = device.usage;
    out_info->input_report_length = device.input_report_length;
    out_info->output_report_length = device.output_report_length;
    out_info->feature_report_length = device.feature_report_length;
    return LOGIPRO_OK;
}

size_t logipro_snapshot_device_count(const logipro_snapshot_t* snapshot) {
    return snapshot == nullptr ? 0 : snapshot->devices.size();
}

int logipro_snapshot_get_device(const logipro_snapshot_t* snapshot, size_t index, logipro_device_info_t* out_info) {
    clear_error();
    if (validate_snapshot(snapshot) != LOGIPRO_OK || out_info == nullptr) return out_info == nullptr ? fail(LOGIPRO_INVALID_ARGUMENT, "Output device info is null.") : LOGIPRO_INVALID_ARGUMENT;
    if (index >= snapshot->devices.size()) return fail(LOGIPRO_NOT_FOUND, "HID++ device index is out of range.");
    const auto& device = snapshot->devices[index];
    const auto& onboard = device.onboard_profiles;
    const auto& lighting = device.lighting;
    const auto& battery = device.battery;
    const auto& dpi = device.dpi;
    *out_info = {};
    out_info->path = snapshot->device_paths[index].c_str();
    out_info->product = snapshot->device_products[index].c_str();
    out_info->product_id = device.product_id;
    out_info->device_index = device.device_index;
    out_info->protocol_major = device.protocol_major;
    out_info->protocol_minor = device.protocol_minor;
    out_info->feature_count = device.features.size();
    out_info->onboard_profiles_readable = onboard.readable ? 1 : 0;
    out_info->onboard_feature_index = onboard.feature_index;
    out_info->onboard_mode_readable = onboard.mode_readable ? 1 : 0;
    out_info->onboard_mode = onboard.mode;
    out_info->profile_format = onboard.profile_format;
    out_info->button_count = onboard.button_count;
    out_info->sector_size = onboard.sector_size;
    out_info->active_profile_readable = onboard.active_profile_readable ? 1 : 0;
    out_info->active_sector = onboard.active_sector;
    out_info->active_profile_crc_valid = onboard.active_profile_crc_valid ? 1 : 0;
    out_info->button_offset = onboard.button_offset;
    out_info->active_lighting_readable = onboard.active_lighting_readable ? 1 : 0;
    out_info->active_lighting_count = onboard.active_lighting.size();
    out_info->dpi_profile_readable = onboard.dpi_profile_readable ? 1 : 0;
    out_info->dpi_profile_count = onboard.dpi_profile_count;
    out_info->dpi_default_index = onboard.dpi_default_index;
    out_info->dpi_shift_index = onboard.dpi_shift_index;
    for (std::size_t dpi_index = 0; dpi_index < onboard.dpi_profile_values.size(); ++dpi_index) out_info->dpi_profile_values[dpi_index] = onboard.dpi_profile_values[dpi_index];
    out_info->dpi_readable = dpi.readable ? 1 : 0;
    out_info->dpi_feature_index = dpi.feature_index;
    out_info->dpi_sensor_count = dpi.sensors.size();
    out_info->lighting_readable = lighting.readable ? 1 : 0;
    out_info->lighting_feature_index = lighting.feature_index;
    out_info->lighting_declared_zone_count = lighting.zone_count;
    out_info->lighting_nv_capabilities = lighting.nv_capabilities;
    out_info->lighting_extended_capabilities = lighting.extended_capabilities;
    out_info->lighting_software_control_readable = lighting.software_control_readable ? 1 : 0;
    out_info->lighting_software_control = lighting.software_control;
    out_info->lighting_sync_events = lighting.sync_events;
    out_info->lighting_zone_records = lighting.zones.size();
    out_info->battery_readable = battery.readable ? 1 : 0;
    out_info->battery_feature_id = battery.feature_id;
    out_info->battery_feature_index = battery.feature_index;
    out_info->battery_percentage_readable = battery.percentage_readable ? 1 : 0;
    out_info->battery_percentage = battery.percentage;
    out_info->battery_percentage_estimated = battery.percentage_estimated ? 1 : 0;
    out_info->battery_voltage_readable = battery.voltage_readable ? 1 : 0;
    out_info->battery_voltage_mv = battery.voltage_mv;
    out_info->battery_status = battery.status;
    out_info->battery_level = battery.level;
    out_info->battery_flags = battery.flags;
    return LOGIPRO_OK;
}

int logipro_snapshot_get_feature(const logipro_snapshot_t* snapshot, size_t device_index, size_t feature_index, logipro_feature_info_t* out_info) {
    clear_error();
    if (validate_snapshot(snapshot) != LOGIPRO_OK || out_info == nullptr) return out_info == nullptr ? fail(LOGIPRO_INVALID_ARGUMENT, "Output feature info is null.") : LOGIPRO_INVALID_ARGUMENT;
    if (device_index >= snapshot->devices.size()) return fail(LOGIPRO_NOT_FOUND, "HID++ device index is out of range.");
    const auto& features = snapshot->devices[device_index].features;
    if (feature_index >= features.size()) return fail(LOGIPRO_NOT_FOUND, "Feature index is out of range.");
    const auto& feature = features[feature_index];
    *out_info = {feature.id, feature.index, static_cast<uint8_t>(feature.present ? 1 : 0), feature.version, feature.type};
    return LOGIPRO_OK;
}

int logipro_snapshot_get_button(const logipro_snapshot_t* snapshot, size_t device_index, uint8_t button, uint8_t out_spec[4]) {
    clear_error();
    if (validate_snapshot(snapshot) != LOGIPRO_OK || out_spec == nullptr) return out_spec == nullptr ? fail(LOGIPRO_INVALID_ARGUMENT, "Output button spec is null.") : LOGIPRO_INVALID_ARGUMENT;
    if (device_index >= snapshot->devices.size()) return fail(LOGIPRO_NOT_FOUND, "HID++ device index is out of range.");
    const auto& buttons = snapshot->devices[device_index].onboard_profiles.buttons;
    if (button == 0 || button > buttons.size()) return fail(LOGIPRO_NOT_FOUND, "Button index is out of range.");
    std::memcpy(out_spec, buttons[button - 1].spec.data(), 4);
    return LOGIPRO_OK;
}

int logipro_snapshot_get_dpi_sensor(const logipro_snapshot_t* snapshot, size_t device_index, size_t sensor_index, logipro_dpi_sensor_info_t* out_info) {
    clear_error();
    if (validate_snapshot(snapshot) != LOGIPRO_OK || out_info == nullptr) return out_info == nullptr ? fail(LOGIPRO_INVALID_ARGUMENT, "Output DPI sensor info is null.") : LOGIPRO_INVALID_ARGUMENT;
    if (device_index >= snapshot->devices.size()) return fail(LOGIPRO_NOT_FOUND, "HID++ device index is out of range.");
    const auto& sensors = snapshot->devices[device_index].dpi.sensors;
    if (sensor_index >= sensors.size()) return fail(LOGIPRO_NOT_FOUND, "DPI sensor index is out of range.");
    const auto& sensor = sensors[sensor_index];
    *out_info = {sensor.index, sensor.current_dpi, sensor.min_dpi, sensor.max_dpi, sensor.step, sensor.default_dpi, sensor.values.size()};
    return LOGIPRO_OK;
}

int logipro_snapshot_get_dpi_value(const logipro_snapshot_t* snapshot, size_t device_index, size_t sensor_index, size_t value_index, uint16_t* out_dpi) {
    clear_error();
    if (validate_snapshot(snapshot) != LOGIPRO_OK || out_dpi == nullptr) return out_dpi == nullptr ? fail(LOGIPRO_INVALID_ARGUMENT, "Output DPI value is null.") : LOGIPRO_INVALID_ARGUMENT;
    if (device_index >= snapshot->devices.size()) return fail(LOGIPRO_NOT_FOUND, "HID++ device index is out of range.");
    const auto& sensors = snapshot->devices[device_index].dpi.sensors;
    if (sensor_index >= sensors.size()) return fail(LOGIPRO_NOT_FOUND, "DPI sensor index is out of range.");
    const auto& values = sensors[sensor_index].values;
    if (value_index >= values.size()) return fail(LOGIPRO_NOT_FOUND, "DPI value index is out of range.");
    *out_dpi = values[value_index];
    return LOGIPRO_OK;
}

int logipro_snapshot_get_active_lighting(const logipro_snapshot_t* snapshot, size_t device_index, size_t lighting_index, uint8_t out_record[11]) {
    clear_error();
    if (validate_snapshot(snapshot) != LOGIPRO_OK || out_record == nullptr) return out_record == nullptr ? fail(LOGIPRO_INVALID_ARGUMENT, "Output lighting record is null.") : LOGIPRO_INVALID_ARGUMENT;
    if (device_index >= snapshot->devices.size()) return fail(LOGIPRO_NOT_FOUND, "HID++ device index is out of range.");
    const auto& lighting = snapshot->devices[device_index].onboard_profiles.active_lighting;
    if (lighting_index >= lighting.size()) return fail(LOGIPRO_NOT_FOUND, "Lighting record index is out of range.");
    std::memcpy(out_record, lighting[lighting_index].data(), 11);
    return LOGIPRO_OK;
}

int logipro_snapshot_get_lighting_zone(const logipro_snapshot_t* snapshot, size_t device_index, size_t zone_index, logipro_lighting_zone_info_t* out_info) {
    clear_error();
    if (validate_snapshot(snapshot) != LOGIPRO_OK || out_info == nullptr) return out_info == nullptr ? fail(LOGIPRO_INVALID_ARGUMENT, "Output lighting zone is null.") : LOGIPRO_INVALID_ARGUMENT;
    if (device_index >= snapshot->devices.size()) return fail(LOGIPRO_NOT_FOUND, "HID++ device index is out of range.");
    const auto& zones = snapshot->devices[device_index].lighting.zones;
    if (zone_index >= zones.size()) return fail(LOGIPRO_NOT_FOUND, "Lighting zone index is out of range.");
    const auto& zone = zones[zone_index];
    *out_info = {};
    out_info->requested_zone = zone.requested_zone;
    out_info->info_readable = zone.zone_info_readable ? 1 : 0;
    out_info->info_zone = zone.info_zone;
    out_info->location = zone.location;
    out_info->effect_count = zone.effect_count;
    out_info->effect_readable = zone.effect_readable ? 1 : 0;
    out_info->effect_zone = zone.effect_zone;
    out_info->effect = zone.effect;
    out_info->settings_readable = zone.settings_readable ? 1 : 0;
    out_info->settings_zone = zone.settings_zone;
    std::memcpy(out_info->effect_parameters, zone.parameters.data(), zone.parameters.size());
    return LOGIPRO_OK;
}

int logipro_snapshot_get_lighting_effect_id(const logipro_snapshot_t* snapshot, size_t device_index, size_t zone_index, size_t effect_index, uint16_t* out_effect_id) {
    clear_error();
    if (validate_snapshot(snapshot) != LOGIPRO_OK || out_effect_id == nullptr) return out_effect_id == nullptr ? fail(LOGIPRO_INVALID_ARGUMENT, "Output lighting effect ID is null.") : LOGIPRO_INVALID_ARGUMENT;
    if (device_index >= snapshot->devices.size()) return fail(LOGIPRO_NOT_FOUND, "HID++ device index is out of range.");
    const auto& zones = snapshot->devices[device_index].lighting.zones;
    if (zone_index >= zones.size()) return fail(LOGIPRO_NOT_FOUND, "Lighting zone index is out of range.");
    const auto& effects = zones[zone_index].effect_ids;
    if (effect_index >= effects.size()) return fail(LOGIPRO_NOT_FOUND, "Lighting effect index is out of range.");
    *out_effect_id = effects[effect_index];
    return LOGIPRO_OK;
}

int logipro_profile_bind(uint8_t button, const uint8_t spec[4]) {
    clear_error();
    if (button == 0 || button > 8 || spec == nullptr) return fail(LOGIPRO_INVALID_ARGUMENT, "Profile button must be 1-8 and its spec must be provided.");
    try {
        std::array<std::uint8_t, 4> value{};
        std::memcpy(value.data(), spec, value.size());
        return operation_result(logipro::bind_onboard_button(button, value));
    } catch (const std::exception& error) {
        return fail(LOGIPRO_INTERNAL_ERROR, error.what());
    } catch (...) {
        return fail(LOGIPRO_INTERNAL_ERROR, "Unknown profile-bind error.");
    }
}

int logipro_dpi_set(uint8_t sensor, uint16_t dpi) {
    clear_error();
    if (dpi == 0) return fail(LOGIPRO_INVALID_ARGUMENT, "DPI must be greater than zero.");
    try {
        return operation_result(logipro::set_dpi(sensor, dpi));
    } catch (const std::exception& error) {
        return fail(LOGIPRO_INTERNAL_ERROR, error.what());
    } catch (...) {
        return fail(LOGIPRO_INTERNAL_ERROR, "Unknown DPI operation error.");
    }
}

int logipro_profile_dpi_set(uint8_t slot, uint16_t dpi) {
    clear_error();
    if (slot >= 5 || dpi == 0) return fail(LOGIPRO_INVALID_ARGUMENT, "DPI slot must be 0-4 and DPI must be greater than zero.");
    try {
        return operation_result(logipro::set_onboard_dpi(slot, dpi));
    } catch (const std::exception& error) {
        return fail(LOGIPRO_INTERNAL_ERROR, error.what());
    } catch (...) {
        return fail(LOGIPRO_INTERNAL_ERROR, "Unknown onboard DPI operation error.");
    }
}

int logipro_profile_dpi_set_default(uint8_t slot) {
    clear_error();
    if (slot >= 5) return fail(LOGIPRO_INVALID_ARGUMENT, "DPI slot must be 0-4.");
    try {
        return operation_result(logipro::set_onboard_default_dpi(slot));
    } catch (const std::exception& error) {
        return fail(LOGIPRO_INTERNAL_ERROR, error.what());
    } catch (...) {
        return fail(LOGIPRO_INTERNAL_ERROR, "Unknown default DPI operation error.");
    }
}

int logipro_lighting_set_effect(uint16_t effect_id, uint16_t period_ms, uint8_t brightness) {
    clear_error();
    if (period_ms == 0 || brightness > 100) return fail(LOGIPRO_INVALID_ARGUMENT, "Lighting period must be greater than zero and brightness must be 0-100.");
    try {
        return operation_result(logipro::set_lighting_effect_settings(effect_id, period_ms, brightness));
    } catch (const std::exception& error) {
        return fail(LOGIPRO_INTERNAL_ERROR, error.what());
    } catch (...) {
        return fail(LOGIPRO_INTERNAL_ERROR, "Unknown lighting-effect operation error.");
    }
}

int logipro_lighting_set_zone_effect(uint8_t zone, uint16_t effect_id, uint16_t period_ms, uint8_t brightness) {
    clear_error();
    if (period_ms == 0 || brightness > 100) return fail(LOGIPRO_INVALID_ARGUMENT, "Lighting period must be greater than zero and brightness must be 0-100.");
    try {
        return operation_result(logipro::set_lighting_zone_effect_settings(zone, effect_id, period_ms, brightness));
    } catch (const std::exception& error) {
        return fail(LOGIPRO_INTERNAL_ERROR, error.what());
    } catch (...) {
        return fail(LOGIPRO_INTERNAL_ERROR, "Unknown zone lighting-effect operation error.");
    }
}

int logipro_lighting_set_software_control(uint8_t enabled) {
    clear_error();
    if (enabled > 1) return fail(LOGIPRO_INVALID_ARGUMENT, "Lighting software control must be 0 or 1.");
    try {
        return operation_result(logipro::set_lighting_software_control(enabled != 0));
    } catch (const std::exception& error) {
        return fail(LOGIPRO_INTERNAL_ERROR, error.what());
    } catch (...) {
        return fail(LOGIPRO_INTERNAL_ERROR, "Unknown lighting-control operation error.");
    }
}

int logipro_profile_restore(void) {
    clear_error();
    try {
        return operation_result(logipro::restore_onboard_profile());
    } catch (const std::exception& error) {
        return fail(LOGIPRO_INTERNAL_ERROR, error.what());
    } catch (...) {
        return fail(LOGIPRO_INTERNAL_ERROR, "Unknown profile-restore error.");
    }
}

int logipro_profile_lighting_off(void) {
    clear_error();
    try {
        return operation_result(logipro::disable_onboard_lighting());
    } catch (const std::exception& error) {
        return fail(LOGIPRO_INTERNAL_ERROR, error.what());
    } catch (...) {
        return fail(LOGIPRO_INTERNAL_ERROR, "Unknown lighting operation error.");
    }
}

int logipro_capture_hid(void) {
    clear_error();
    try {
        return operation_result(logipro::capture_hid_reports());
    } catch (const std::exception& error) {
        return fail(LOGIPRO_INTERNAL_ERROR, error.what());
    } catch (...) {
        return fail(LOGIPRO_INTERNAL_ERROR, "Unknown HID capture error.");
    }
}

int logipro_capture_raw_input(void) {
    clear_error();
    try {
        return operation_result(logipro::capture_raw_input());
    } catch (const std::exception& error) {
        return fail(LOGIPRO_INTERNAL_ERROR, error.what());
    } catch (...) {
        return fail(LOGIPRO_INTERNAL_ERROR, "Unknown Raw Input capture error.");
    }
}

int logipro_watch_buttons(void) {
    clear_error();
    try {
        return operation_result(logipro::watch_mouse_buttons());
    } catch (const std::exception& error) {
        return fail(LOGIPRO_INTERNAL_ERROR, error.what());
    } catch (...) {
        return fail(LOGIPRO_INTERNAL_ERROR, "Unknown mouse-watch error.");
    }
}

int logipro_run_mouse_bindings(const logipro_mouse_binding_t* bindings, size_t count) {
    clear_error();
    if (count == 0) return fail(LOGIPRO_INVALID_ARGUMENT, "At least one mouse binding is required.");
    if (bindings == nullptr) return fail(LOGIPRO_INVALID_ARGUMENT, "Mouse bindings are null.");
    try {
        std::vector<logipro::MouseBinding> values;
        values.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            logipro::MouseButton button{};
            switch (bindings[index].button) {
            case LOGIPRO_MOUSE_LEFT: button = logipro::MouseButton::Left; break;
            case LOGIPRO_MOUSE_RIGHT: button = logipro::MouseButton::Right; break;
            case LOGIPRO_MOUSE_MIDDLE: button = logipro::MouseButton::Middle; break;
            case LOGIPRO_MOUSE_BACK: button = logipro::MouseButton::Back; break;
            case LOGIPRO_MOUSE_FORWARD: button = logipro::MouseButton::Forward; break;
            default: return fail(LOGIPRO_INVALID_ARGUMENT, "Unknown mouse button.");
            }
            values.push_back({button, bindings[index].virtual_key});
        }
        return operation_result(logipro::run_mouse_bindings(values));
    } catch (const std::exception& error) {
        return fail(LOGIPRO_INTERNAL_ERROR, error.what());
    } catch (...) {
        return fail(LOGIPRO_INTERNAL_ERROR, "Unknown mouse-binding error.");
    }
}

void logipro_debug_set_enabled(int enabled) {
    logipro::set_debug_enabled(enabled != 0);
}

int logipro_debug_is_enabled(void) {
    return logipro::debug_enabled() ? 1 : 0;
}

const char* logipro_last_error(void) {
    return last_error.c_str();
}

}
