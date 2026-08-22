#pragma once

#include "logipro/hid.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace logipro {

struct HidppFeatureInfo {
    std::uint16_t id = 0;
    std::uint8_t index = 0;
    bool present = false;
    std::uint8_t version = 0;
    std::uint8_t type = 0;
};

struct HidppOnboardProfileHeader {
    std::uint16_t sector = 0;
    std::uint8_t enabled = 0;
};

struct HidppOnboardButtonInfo {
    std::array<std::uint8_t, 4> spec{};
};

struct HidppOnboardProfileInfo {
    bool readable = false;
    std::uint8_t feature_index = 0;
    bool mode_readable = false;
    std::uint8_t mode = 0;
    std::vector<std::uint8_t> info;
    std::uint8_t profile_format = 0;
    std::uint8_t button_count = 0;
    std::uint16_t sector_size = 0;
    std::vector<HidppOnboardProfileHeader> profiles;
    bool active_profile_readable = false;
    std::uint16_t active_sector = 0;
    bool active_profile_crc_valid = false;
    std::uint8_t button_offset = 0;
    std::vector<HidppOnboardButtonInfo> buttons;
    bool active_lighting_readable = false;
    std::vector<std::array<std::uint8_t, 11>> active_lighting;
    bool dpi_profile_readable = false;
    std::uint8_t dpi_profile_count = 0;
    std::uint8_t dpi_default_index = 0;
    std::uint8_t dpi_shift_index = 0;
    std::array<std::uint16_t, 5> dpi_profile_values{};
};

struct HidppLightingZoneInfo {
    std::uint8_t requested_zone = 0;
    bool zone_info_readable = false;
    std::uint8_t info_zone = 0;
    std::uint16_t location = 0;
    std::uint8_t effect_count = 0;
    std::vector<std::uint16_t> effect_ids;
    bool effect_readable = false;
    std::uint8_t effect_zone = 0;
    std::uint8_t zone = 0;
    std::uint8_t effect = 0;
    std::array<std::uint8_t, 10> parameters{};
    bool settings_readable = false;
    std::uint8_t settings_zone = 0;
    std::array<std::uint8_t, 8> settings{};
};

struct HidppLightingInfo {
    bool readable = false;
    std::uint8_t feature_index = 0;
    std::uint8_t zone_count = 0;
    std::uint16_t nv_capabilities = 0;
    std::uint16_t extended_capabilities = 0;
    bool software_control_readable = false;
    std::uint8_t software_control = 0;
    std::uint8_t sync_events = 0;
    std::vector<HidppLightingZoneInfo> zones;
};

struct HidppBatteryInfo {
    bool readable = false;
    std::uint16_t feature_id = 0;
    std::uint8_t feature_index = 0;
    bool percentage_readable = false;
    std::uint8_t percentage = 0;
    bool percentage_estimated = false;
    bool voltage_readable = false;
    std::uint16_t voltage_mv = 0;
    std::uint8_t status = 0;
    std::uint8_t level = 0;
    std::uint8_t flags = 0;
};

struct HidppDpiSensorInfo {
    std::uint8_t index = 0;
    std::uint16_t current_dpi = 0;
    std::uint16_t min_dpi = 0;
    std::uint16_t max_dpi = 0;
    std::uint16_t step = 0;
    std::uint16_t default_dpi = 0;
    std::vector<std::uint16_t> values;
};

struct HidppDpiInfo {
    bool readable = false;
    std::uint8_t feature_index = 0;
    std::vector<HidppDpiSensorInfo> sensors;
};

struct HidppDeviceInfo {
    std::wstring path;
    std::wstring product;
    std::uint16_t product_id = 0;
    std::uint8_t device_index = 0;
    std::uint8_t protocol_major = 0;
    std::uint8_t protocol_minor = 0;
    std::vector<HidppFeatureInfo> features;
    HidppOnboardProfileInfo onboard_profiles;
    HidppLightingInfo lighting;
    HidppBatteryInfo battery;
    HidppDpiInfo dpi;
};

std::vector<HidppDeviceInfo> probe_logitech_hidpp(const std::vector<HidDeviceInfo>& interfaces);
int bind_onboard_button(std::uint8_t button, const std::array<std::uint8_t, 4>& spec);
int set_dpi(std::uint8_t sensor, std::uint16_t dpi);
int set_onboard_dpi(std::uint8_t slot, std::uint16_t dpi);
int set_onboard_default_dpi(std::uint8_t slot);
int set_lighting_effect(std::uint16_t effect_id);
int set_lighting_effect_settings(std::uint16_t effect_id, std::uint16_t period_ms, std::uint8_t brightness);
int set_lighting_zone_effect_settings(std::uint8_t zone, std::uint16_t effect_id, std::uint16_t period_ms, std::uint8_t brightness);
int set_lighting_software_control(bool enabled);
int restore_onboard_profile();
int disable_onboard_lighting();

}
