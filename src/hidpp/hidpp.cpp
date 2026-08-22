#include "logipro/hidpp.hpp"

#include "debug.hpp"

#ifdef _WIN32

#include <windows.h>
#include <hidsdi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <streambuf>
#include <utility>

namespace {

class NullBuffer : public std::streambuf {
protected:
    int_type overflow(int_type character) override { return traits_type::not_eof(character); }
};

class NullWideBuffer : public std::wstreambuf {
protected:
    int_type overflow(int_type character) override { return traits_type::not_eof(character); }
};

class ScopedOutputSilencer {
public:
    ScopedOutputSilencer() {
        if (logipro::debug_enabled()) return;
        old_cout_ = std::cout.rdbuf(&null_);
        old_cerr_ = std::cerr.rdbuf(&null_);
        old_wcout_ = std::wcout.rdbuf(&wide_null_);
        old_wcerr_ = std::wcerr.rdbuf(&wide_null_);
        silenced_ = true;
    }
    ~ScopedOutputSilencer() {
        if (!silenced_) return;
        std::cout.rdbuf(old_cout_);
        std::cerr.rdbuf(old_cerr_);
        std::wcout.rdbuf(old_wcout_);
        std::wcerr.rdbuf(old_wcerr_);
    }

private:
    NullBuffer null_;
    NullWideBuffer wide_null_;
    std::streambuf* old_cout_ = nullptr;
    std::streambuf* old_cerr_ = nullptr;
    std::wstreambuf* old_wcout_ = nullptr;
    std::wstreambuf* old_wcerr_ = nullptr;
    bool silenced_ = false;
};

constexpr std::uint8_t short_report_id = 0x10;
constexpr std::uint8_t long_report_id = 0x11;
constexpr std::size_t long_report_length = 20;
constexpr std::uint8_t direct_device_index = 0xff;
constexpr std::uint8_t software_id = 1;
constexpr std::uint16_t receiver_product_id = 0xc539;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~ScopedHandle() {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
            CloseHandle(value_);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const { return value_; }
    bool valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }

private:
    HANDLE value_;
};

std::optional<ScopedHandle> open_connection(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    return std::optional<ScopedHandle>(std::in_place, handle);
}

std::vector<std::uint8_t> pad_report(const std::vector<std::uint8_t>& report, std::size_t length) {
    const std::size_t size = std::max(report.size(), length);
    std::vector<std::uint8_t> padded(size);
    std::copy(report.begin(), report.end(), padded.begin());
    return padded;
}

bool wait_for_io(HANDLE handle, OVERLAPPED& overlapped, DWORD timeout) {
    const DWORD wait = WaitForSingleObject(overlapped.hEvent, timeout);
    if (wait == WAIT_OBJECT_0) {
        return true;
    }
    CancelIoEx(handle, &overlapped);
    WaitForSingleObject(overlapped.hEvent, INFINITE);
    return false;
}

bool write_report(HANDLE handle, const std::vector<std::uint8_t>& report, const logipro::HidDeviceInfo& device) {
    const std::size_t output_length = device.output_report_length == 0 ? long_report_length : device.output_report_length;
    auto output = pad_report(report, output_length);
    ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD written = 0;
    BOOL complete = WriteFile(handle, output.data(), static_cast<DWORD>(output.size()), &written, &overlapped);
    if (!complete && GetLastError() == ERROR_IO_PENDING) {
        complete = wait_for_io(handle, overlapped, 1000) && GetOverlappedResult(handle, &overlapped, &written, FALSE);
    }
    if (complete && written == output.size()) {
        return true;
    }
    if (HidD_SetOutputReport(handle, output.data(), static_cast<ULONG>(output.size()))) {
        return true;
    }
    if (device.feature_report_length == 0) {
        return false;
    }
    auto feature = pad_report(report, device.feature_report_length);
    return HidD_SetFeature(handle, feature.data(), static_cast<ULONG>(feature.size())) != FALSE;
}

std::optional<std::vector<std::uint8_t>> read_report(HANDLE handle, const logipro::HidDeviceInfo& device, DWORD timeout) {
    const std::size_t input_length = std::max<std::size_t>(device.input_report_length, long_report_length);
    std::vector<std::uint8_t> input(input_length);
    ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        return std::nullopt;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD read = 0;
    BOOL complete = ReadFile(handle, input.data(), static_cast<DWORD>(input.size()), &read, &overlapped);
    if (!complete && GetLastError() == ERROR_IO_PENDING) {
        complete = wait_for_io(handle, overlapped, timeout) && GetOverlappedResult(handle, &overlapped, &read, FALSE);
    }
    if (!complete || read < 4) {
        return std::nullopt;
    }
    input.resize(read);
    return input;
}

std::optional<std::vector<std::uint8_t>> call(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index, std::uint8_t feature_index, std::uint8_t function, const std::vector<std::uint8_t>& parameters) {
    const std::uint8_t function_sw_id = static_cast<std::uint8_t>((function & 0xf0) | software_id);
    std::vector<std::uint8_t> request(long_report_length);
    request[0] = long_report_id;
    request[1] = device_index;
    request[2] = feature_index;
    request[3] = function_sw_id;
    std::copy(parameters.begin(), parameters.end(), request.begin() + 4);
    if (!write_report(handle, request, device)) {
        return std::nullopt;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        const auto response = read_report(handle, device, static_cast<DWORD>(std::max<std::int64_t>(1, remaining)));
        if (!response) {
            return std::nullopt;
        }
        if ((response->at(0) != short_report_id && response->at(0) != long_report_id) || response->at(1) != device_index) {
            continue;
        }
        if (response->size() < 5) continue;
        if (response->at(2) == 0xff) {
            if (response->at(3) == feature_index && response->at(4) == function_sw_id) return std::nullopt;
            continue;
        }
        if (response->at(2) != feature_index || response->at(3) != function_sw_id) {
            continue;
        }
        return std::vector<std::uint8_t>(response->begin() + 4, response->end());
    }
    return std::nullopt;
}

std::optional<std::uint8_t> get_feature_index(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index, std::uint16_t feature_id) {
    const std::vector<std::uint8_t> parameters = {static_cast<std::uint8_t>(feature_id >> 8), static_cast<std::uint8_t>(feature_id)};
    const auto response = call(handle, device, device_index, 0x00, 0x00, parameters);
    if (!response || response->empty()) {
        return std::nullopt;
    }
    return response->front();
}

std::vector<logipro::HidppFeatureInfo> enumerate_feature_set(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index) {
    std::vector<logipro::HidppFeatureInfo> features;
    const auto feature_set_index = get_feature_index(handle, device, device_index, 0x0001);
    if (!feature_set_index) return features;
    const auto count_response = call(handle, device, device_index, *feature_set_index, 0x00, {0x00, 0x00, 0x00});
    if (!count_response || count_response->empty()) return features;
    for (std::uint16_t entry = 1; entry <= count_response->front(); ++entry) {
        const auto response = call(handle, device, device_index, *feature_set_index, 0x10, {static_cast<std::uint8_t>(entry), 0x00, 0x00});
        if (!response || response->size() < 4) continue;
        features.push_back({static_cast<std::uint16_t>(response->at(0) << 8 | response->at(1)), static_cast<std::uint8_t>(entry), true, response->at(3), response->at(2)});
    }
    return features;
}

std::optional<std::vector<std::uint8_t>> read_sector(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index, std::uint8_t feature_index, std::uint16_t sector, std::size_t size) {
    if (size < 16) return std::nullopt;
    std::vector<std::uint8_t> output;
    output.reserve(size);
    const std::uint8_t sector_high = static_cast<std::uint8_t>(sector >> 8);
    const std::uint8_t sector_low = static_cast<std::uint8_t>(sector);
    std::size_t offset = 0;
    while (offset + 16 <= size) {
        const auto response = call(handle, device, device_index, feature_index, 0x50, {sector_high, sector_low, static_cast<std::uint8_t>(offset >> 8), static_cast<std::uint8_t>(offset)});
        if (!response || response->size() < 16) return std::nullopt;
        output.insert(output.end(), response->begin(), response->begin() + 16);
        offset += 16;
    }
    if (offset < size) {
        const std::size_t read_offset = size - 16;
        const auto response = call(handle, device, device_index, feature_index, 0x50, {sector_high, sector_low, static_cast<std::uint8_t>(read_offset >> 8), static_cast<std::uint8_t>(read_offset)});
        if (!response || response->size() < 16 || read_offset > offset) return std::nullopt;
        const std::size_t skip = 16 + offset - size;
        if (skip > 16) return std::nullopt;
        output.insert(output.end(), response->begin() + skip, response->begin() + 16);
    }
    output.resize(size);
    return output;
}

std::uint16_t crc16_ccitt_false(const std::vector<std::uint8_t>& data, std::size_t length) {
    std::uint16_t crc = 0xffff;
    for (std::size_t index = 0; index < length; ++index) {
        crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(data[index]) << 8));
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<std::uint16_t>((crc & 0x8000) != 0 ? (crc << 1) ^ 0x1021 : crc << 1);
        }
    }
    return crc;
}

bool valid_sector_crc(const std::vector<std::uint8_t>& sector) {
    if (sector.size() < 2) return false;
    const std::uint16_t stored = static_cast<std::uint16_t>(sector[sector.size() - 2] << 8 | sector[sector.size() - 1]);
    return crc16_ccitt_false(sector, sector.size() - 2) == stored;
}

std::optional<logipro::HidppLightingInfo> read_lighting_state(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index, std::uint8_t feature_index) {
    const auto info = call(handle, device, device_index, feature_index, 0x00, {0x00, 0x00, 0x00});
    if (!info || info->size() < 5) return std::nullopt;
    logipro::HidppLightingInfo result;
    result.readable = true;
    result.feature_index = feature_index;
    result.zone_count = info->at(0);
    result.nv_capabilities = static_cast<std::uint16_t>(info->at(1) << 8 | info->at(2));
    result.extended_capabilities = static_cast<std::uint16_t>(info->at(3) << 8 | info->at(4));
    const auto control = call(handle, device, device_index, feature_index, 0x70, {0x00, 0x00, 0x00});
    if (!control || control->size() < 2) return result;
    result.software_control_readable = true;
    result.software_control = control->at(0);
    result.sync_events = control->at(1);
    for (std::uint8_t zone = 0; zone < result.zone_count; ++zone) {
        logipro::HidppLightingZoneInfo state;
        state.requested_zone = zone;
        const auto zone_info = call(handle, device, device_index, feature_index, 0x10, {zone, 0x00, 0x00});
        if (zone_info && zone_info->size() >= 4) {
            state.zone_info_readable = true;
            state.info_zone = zone_info->at(0);
            state.location = static_cast<std::uint16_t>(zone_info->at(1) << 8 | zone_info->at(2));
            state.effect_count = zone_info->at(3);
            for (std::uint8_t effect_index = 0; effect_index < state.effect_count; ++effect_index) {
                const auto effect_info = call(handle, device, device_index, feature_index, 0x20, {zone, effect_index, 0x00});
                if (!effect_info || effect_info->size() < 4) continue;
                state.effect_ids.push_back(static_cast<std::uint16_t>(effect_info->at(2) << 8 | effect_info->at(3)));
            }
            state.effect_count = static_cast<std::uint8_t>(state.effect_ids.size());
        }
        if ((result.extended_capabilities & 0x0001) != 0) {
            const auto effect = call(handle, device, device_index, feature_index, 0xe0, {zone, 0x00, 0x00});
            if (effect && effect->size() >= 12) {
                state.effect_readable = true;
                state.effect_zone = effect->at(0);
                state.zone = state.effect_zone;
                state.effect = effect->at(1);
                std::copy(effect->begin() + 2, effect->begin() + 12, state.parameters.begin());
            }
        } else {
            const auto settings = call(handle, device, device_index, feature_index, 0x90, {zone, 0x00, 0x00});
            if (settings && settings->size() >= 8) {
                state.settings_readable = true;
                state.settings_zone = settings->at(0);
                std::copy(settings->begin(), settings->begin() + 8, state.settings.begin());
            }
        }
        result.zones.push_back(state);
    }
    return result;
}

std::optional<logipro::HidppBatteryInfo> read_battery_state(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index, const std::vector<logipro::HidppFeatureInfo>& features) {
    const auto find_feature = [&features](std::uint16_t id) {
        return std::find_if(features.begin(), features.end(), [id](const auto& feature) { return feature.id == id && feature.present; });
    };
    const auto unified_feature = find_feature(0x1004);
    if (unified_feature != features.end()) {
        const auto capabilities = call(handle, device, device_index, unified_feature->index, 0x00, {0x00, 0x00, 0x00});
        const auto info = call(handle, device, device_index, unified_feature->index, 0x10, {0x00, 0x00, 0x00});
        if (capabilities && capabilities->size() >= 2 && info && info->size() >= 3) {
            logipro::HidppBatteryInfo result;
            result.readable = true;
            result.feature_id = 0x1004;
            result.feature_index = unified_feature->index;
            result.percentage_readable = (capabilities->at(1) & 0x02) != 0;
            result.percentage = info->at(0);
            result.level = info->at(1);
            result.status = info->at(2);
            return result;
        }
    }
    const auto status_feature = find_feature(0x1000);
    if (status_feature != features.end()) {
        const auto info = call(handle, device, device_index, status_feature->index, 0x00, {0x00, 0x00, 0x00});
        if (info && info->size() >= 3) {
            logipro::HidppBatteryInfo result;
            result.readable = true;
            result.feature_id = 0x1000;
            result.feature_index = status_feature->index;
            result.percentage_readable = true;
            result.percentage = info->at(0);
            result.level = info->at(1);
            result.status = info->at(2);
            return result;
        }
    }
    const auto voltage_feature = find_feature(0x1001);
    if (voltage_feature == features.end()) return std::nullopt;
    const auto voltage = call(handle, device, device_index, voltage_feature->index, 0x00, {0x00, 0x00, 0x00});
    if (!voltage || voltage->size() < 3) return std::nullopt;
    logipro::HidppBatteryInfo result;
    result.readable = true;
    result.feature_id = 0x1001;
    result.feature_index = voltage_feature->index;
    result.voltage_readable = true;
    result.voltage_mv = static_cast<std::uint16_t>(voltage->at(0) << 8 | voltage->at(1));
    result.flags = voltage->at(2);
    result.status = result.flags;
    result.percentage_readable = true;
    result.percentage_estimated = true;
    result.percentage = static_cast<std::uint8_t>(std::clamp((static_cast<int>(result.voltage_mv) - 3500) * 100 / 700, 0, 100));
    return result;
}

std::optional<logipro::HidppDpiInfo> read_dpi_state(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index, std::uint8_t feature_index) {
    const auto count = call(handle, device, device_index, feature_index, 0x00, {0x00, 0x00, 0x00});
    if (!count || count->empty() || count->front() == 0 || count->front() > 8) return std::nullopt;
    logipro::HidppDpiInfo result;
    result.readable = true;
    result.feature_index = feature_index;
    for (std::uint8_t sensor_index = 0; sensor_index < count->front(); ++sensor_index) {
        const auto list = call(handle, device, device_index, feature_index, 0x10, {sensor_index, 0x00, 0x00});
        const auto current = call(handle, device, device_index, feature_index, 0x20, {sensor_index, 0x00, 0x00});
        if (!list || list->size() < 3 || !current || current->size() < 5) continue;
        logipro::HidppDpiSensorInfo sensor;
        sensor.index = list->at(0);
        for (std::size_t offset = 1; offset + 1 < list->size(); offset += 2) {
            const std::uint16_t value = static_cast<std::uint16_t>(list->at(offset) << 8 | list->at(offset + 1));
            if (value == 0) break;
            if (value > 0xe000) {
                sensor.step = static_cast<std::uint16_t>(value - 0xe000);
            } else {
                sensor.values.push_back(value);
            }
        }
        sensor.current_dpi = static_cast<std::uint16_t>(current->at(1) << 8 | current->at(2));
        sensor.default_dpi = static_cast<std::uint16_t>(current->at(3) << 8 | current->at(4));
        if (!sensor.values.empty()) {
            sensor.min_dpi = *std::min_element(sensor.values.begin(), sensor.values.end());
            sensor.max_dpi = *std::max_element(sensor.values.begin(), sensor.values.end());
        }
        result.sensors.push_back(std::move(sensor));
    }
    return result.sensors.empty() ? std::nullopt : std::optional<logipro::HidppDpiInfo>(std::move(result));
}

bool set_dpi_state(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index, std::uint8_t feature_index, std::uint8_t sensor, std::uint16_t dpi) {
    const auto response = call(handle, device, device_index, feature_index, 0x30, {sensor, static_cast<std::uint8_t>(dpi >> 8), static_cast<std::uint8_t>(dpi)});
    if (!response) return false;
    if (response->size() >= 3) {
        const std::uint16_t echoed = static_cast<std::uint16_t>(response->at(1) << 8 | response->at(2));
        if (echoed != 0 && echoed != dpi) return false;
    }
    return true;
}

bool dpi_supported(const logipro::HidppDpiSensorInfo& sensor, std::uint16_t dpi) {
    if ((sensor.min_dpi != 0 && dpi < sensor.min_dpi) || (sensor.max_dpi != 0 && dpi > sensor.max_dpi)) return false;
    if (sensor.step != 0 && sensor.min_dpi != 0 && (dpi - sensor.min_dpi) % sensor.step != 0) return false;
    if (sensor.step == 0 && !sensor.values.empty() && std::find(sensor.values.begin(), sensor.values.end(), dpi) == sensor.values.end()) return false;
    return true;
}

void set_sector_crc(std::vector<std::uint8_t>& sector) {
    const auto crc = crc16_ccitt_false(sector, sector.size() - 2);
    sector[sector.size() - 2] = static_cast<std::uint8_t>(crc >> 8);
    sector[sector.size() - 1] = static_cast<std::uint8_t>(crc);
}

bool write_sector(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index, std::uint8_t feature_index, std::uint16_t sector, const std::vector<std::uint8_t>& data) {
    const std::vector<std::uint8_t> start_parameters = {static_cast<std::uint8_t>(sector >> 8), static_cast<std::uint8_t>(sector), 0x00, 0x00, static_cast<std::uint8_t>(data.size() >> 8), static_cast<std::uint8_t>(data.size())};
    if (!call(handle, device, device_index, feature_index, 0x60, start_parameters)) return false;
    for (std::size_t offset = 0; offset < data.size(); offset += 16) {
        const auto end = std::min(offset + 16, data.size());
        if (!call(handle, device, device_index, feature_index, 0x70, std::vector<std::uint8_t>(data.begin() + offset, data.begin() + end))) return false;
    }
    return call(handle, device, device_index, feature_index, 0x80, {}).has_value();
}

bool lighting_state_complete(const logipro::HidppLightingInfo& lighting) {
    if (!lighting.readable || !lighting.software_control_readable || lighting.zones.size() != lighting.zone_count) return false;
    for (std::size_t index = 0; index < lighting.zones.size(); ++index) {
        if (lighting.zones[index].requested_zone != index || !lighting.zones[index].zone_info_readable || lighting.zones[index].info_zone != index) return false;
        if (lighting.extended_capabilities & 0x0001) {
            if (!lighting.zones[index].effect_readable || lighting.zones[index].effect_zone != index) return false;
        } else return false;
    }
    return true;
}

bool restore_lighting_state(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index, const logipro::HidppLightingInfo& lighting) {
    if (!lighting_state_complete(lighting)) return false;
    if (!call(handle, device, device_index, lighting.feature_index, 0x80, {0x01, lighting.sync_events, 0x00})) return false;
    for (const auto& zone : lighting.zones) {
        std::vector<std::uint8_t> parameters(13);
        parameters[0] = zone.requested_zone;
        parameters[1] = zone.effect;
        std::copy(zone.parameters.begin(), zone.parameters.end(), parameters.begin() + 2);
        if (!call(handle, device, device_index, lighting.feature_index, 0x30, parameters)) return false;
    }
    return call(handle, device, device_index, lighting.feature_index, 0x80, {lighting.software_control, lighting.sync_events, 0x00}).has_value();
}

bool set_lighting_effect_state(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index, const logipro::HidppLightingInfo& lighting, std::uint16_t effect_id, std::uint16_t period_ms, std::uint8_t brightness) {
    if (!lighting.readable || !lighting.software_control_readable || lighting.zones.size() != lighting.zone_count) return false;
    for (const auto& zone : lighting.zones) if (std::find(zone.effect_ids.begin(), zone.effect_ids.end(), effect_id) == zone.effect_ids.end()) return false;
    if (!call(handle, device, device_index, lighting.feature_index, 0x80, {0x01, lighting.sync_events, 0x00})) return false;
    bool success = true;
    const auto period = static_cast<std::uint16_t>(std::clamp<unsigned int>(period_ms, 100, 60000));
    const auto intensity = brightness >= 100 ? 0 : brightness;
    for (const auto& zone : lighting.zones) {
        std::vector<std::uint8_t> parameters(13);
        parameters[0] = zone.requested_zone;
        parameters[1] = static_cast<std::uint8_t>(std::distance(zone.effect_ids.begin(), std::find(zone.effect_ids.begin(), zone.effect_ids.end(), effect_id)));
        if (effect_id == 1 || effect_id == 10 || effect_id == 11) {
            parameters[2] = 0xff;
            parameters[3] = 0x80;
            parameters[4] = 0x20;
        }
        if (effect_id == 3 || effect_id == 10) {
            parameters[7] = static_cast<std::uint8_t>(period >> 8);
            parameters[8] = static_cast<std::uint8_t>(period);
            parameters[9] = intensity;
        } else if (effect_id == 11) {
            parameters[7] = static_cast<std::uint8_t>(period >> 8);
            parameters[8] = static_cast<std::uint8_t>(period);
        }
        parameters[12] = 1;
        if (!call(handle, device, device_index, lighting.feature_index, 0x30, parameters)) {
            success = false;
            break;
        }
    }
    const bool released = call(handle, device, device_index, lighting.feature_index, 0x80, {lighting.software_control, lighting.sync_events, 0x00}).has_value();
    return success && released;
}

bool set_lighting_effect(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index, const logipro::HidppLightingInfo& lighting, std::uint16_t effect_id) {
    return set_lighting_effect_state(handle, device, device_index, lighting, effect_id, 8000, 100);
}

bool set_lighting_software_control_state(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index, const logipro::HidppLightingInfo& lighting, bool enabled) {
    if (!lighting.readable || !lighting.software_control_readable) return false;
    return call(handle, device, device_index, lighting.feature_index, 0x80, {static_cast<std::uint8_t>(enabled ? 1 : 0), lighting.sync_events, 0x00}).has_value();
}

std::string backup_path(std::uint16_t sector) {
    return "logipro-backup-sector-" + std::to_string(sector) + ".bin";
}

bool save_backup(const std::string& path, const std::vector<std::uint8_t>& data, const std::vector<std::pair<std::size_t, std::size_t>>& mutable_ranges) {
    std::ifstream existing(path, std::ios::binary);
    if (existing.good()) {
        const std::vector<std::uint8_t> old((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
        if (old.size() != data.size() || !valid_sector_crc(old)) return false;
        for (std::size_t index = 0; index < data.size(); ++index) {
            const bool mutable_byte = std::any_of(mutable_ranges.begin(), mutable_ranges.end(), [index](const auto& range) { return index >= range.first && index < range.second; });
            if (!mutable_byte && old[index] != data[index]) return false;
        }
        return true;
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return output.good();
}

bool save_backup(const std::string& path, const std::vector<std::uint8_t>& data, std::uint8_t button_offset, std::uint8_t button_count) {
    const std::size_t button_end = button_offset + static_cast<std::size_t>(button_count) * 4;
    return save_backup(path, data, {{button_offset, button_end}});
}

void print_spec(const std::array<std::uint8_t, 4>& spec) {
    for (const auto byte : spec) {
        std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    }
    std::cout << std::dec << std::setfill(' ');
}

std::optional<std::array<std::uint8_t, 4>> read_button_spec(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index, const logipro::HidppOnboardProfileInfo& onboard, std::uint8_t button, std::vector<std::uint8_t>& profile) {
    if (button < 1 || button > onboard.button_count) return std::nullopt;
    const auto data = read_sector(handle, device, device_index, onboard.feature_index, onboard.active_sector, onboard.sector_size);
    if (!data || !valid_sector_crc(*data)) return std::nullopt;
    profile = *data;
    const std::size_t offset = onboard.button_offset + (button - 1) * 4;
    if (offset + 4 > profile.size()) return std::nullopt;
    std::array<std::uint8_t, 4> spec{};
    std::copy(profile.begin() + offset, profile.begin() + offset + 4, spec.begin());
    return spec;
}

std::optional<logipro::HidppOnboardProfileInfo> read_onboard_profiles(HANDLE handle, const logipro::HidDeviceInfo& device, std::uint8_t device_index, std::uint8_t feature_index) {
    const auto info = call(handle, device, device_index, feature_index, 0x00, {});
    if (!info || info->size() < 9) return std::nullopt;
    logipro::HidppOnboardProfileInfo result;
    result.readable = true;
    result.feature_index = feature_index;
    result.info = *info;
    result.profile_format = info->at(1);
    result.button_count = info->at(5);
    result.sector_size = static_cast<std::uint16_t>(info->at(7) << 8 | info->at(8));
    if (const auto mode = call(handle, device, device_index, feature_index, 0x20, {}); mode && !mode->empty()) {
        result.mode_readable = true;
        result.mode = mode->front();
    }
    if (result.sector_size < 16 || result.sector_size > 4096) return result;
    const auto control = read_sector(handle, device, device_index, feature_index, 0, std::max<std::size_t>(64, result.sector_size));
    if (!control) return result;
    for (std::size_t offset = 0; offset + 3 < control->size(); offset += 4) {
        const std::uint16_t sector = static_cast<std::uint16_t>(control->at(offset) << 8 | control->at(offset + 1));
        if ((control->at(offset) == 0xff && control->at(offset + 1) == 0xff) || sector == 0) break;
        result.profiles.push_back({sector, control->at(offset + 2)});
    }
    const auto active = std::find_if(result.profiles.begin(), result.profiles.end(), [](const auto& profile) { return profile.enabled != 0; });
    const auto selected = active == result.profiles.end() ? (result.profiles.empty() ? result.profiles.end() : result.profiles.begin()) : active;
    if (selected == result.profiles.end()) return result;
    result.active_sector = selected->sector;
    const auto profile = read_sector(handle, device, device_index, feature_index, result.active_sector, result.sector_size);
    if (!profile) return result;
    result.active_profile_readable = true;
    result.active_profile_crc_valid = valid_sector_crc(*profile);
    if (result.profile_format <= 5 && profile->size() >= 13) {
        result.dpi_profile_readable = true;
        result.dpi_profile_count = 5;
        result.dpi_default_index = profile->at(1);
        result.dpi_shift_index = profile->at(2);
        for (std::size_t index = 0; index < result.dpi_profile_values.size(); ++index) {
            const std::size_t offset = 3 + index * 2;
            result.dpi_profile_values[index] = static_cast<std::uint16_t>(profile->at(offset) | profile->at(offset + 1) << 8);
        }
    }
    result.button_offset = result.profile_format >= 6 ? 48 : 32;
    if (result.profile_format == 3 && profile->size() >= 252) {
        result.active_lighting_readable = true;
        for (std::size_t index = 0; index < 4; ++index) {
            std::array<std::uint8_t, 11> lighting{};
            std::copy(profile->begin() + 208 + index * lighting.size(), profile->begin() + 208 + (index + 1) * lighting.size(), lighting.begin());
            result.active_lighting.push_back(lighting);
        }
    }
    const std::size_t buttons_end = static_cast<std::size_t>(result.button_offset) + static_cast<std::size_t>(result.button_count) * 4;
    if (buttons_end > profile->size()) return result;
    for (std::size_t index = 0; index < result.button_count; ++index) {
        const std::size_t offset = result.button_offset + index * 4;
        logipro::HidppOnboardButtonInfo button;
        std::copy(profile->begin() + offset, profile->begin() + offset + 4, button.spec.begin());
        result.buttons.push_back(button);
    }
    return result;
}

std::optional<logipro::HidppDeviceInfo> probe_interface(const logipro::HidDeviceInfo& interface, std::uint8_t device_index) {
    if (interface.usage_page != 0xff00 || std::max({interface.input_report_length, interface.output_report_length, interface.feature_report_length}) < long_report_length) {
        if (logipro::debug_enabled()) std::wcerr << L"HID++ skip " << interface.path << L" usage=0x" << std::hex << interface.usage_page << L" reports=" << std::dec << std::max({interface.input_report_length, interface.output_report_length, interface.feature_report_length}) << L'\n';
        return std::nullopt;
    }
    const auto connection = open_connection(interface.path);
    if (!connection) {
        if (logipro::debug_enabled()) std::wcerr << L"HID++ open failed " << interface.path << L"\n";
        return std::nullopt;
    }
    const std::vector<std::uint8_t> ping_parameters = {0x00, 0x00, 0xaa};
    const auto ping = call(connection->get(), interface, device_index, 0x00, 0x10, ping_parameters);
    if (!ping || ping->size() < 2) {
        if (logipro::debug_enabled()) std::wcerr << L"HID++ ping failed " << interface.path << L" index=" << static_cast<unsigned>(device_index) << L"\n";
        return std::nullopt;
    }
    if (logipro::debug_enabled()) std::wcerr << L"HID++ ping ok " << interface.path << L" index=" << static_cast<unsigned>(device_index) << L"\n";

    logipro::HidppDeviceInfo result;
    result.path = interface.path;
    result.product = interface.product;
    result.product_id = interface.product_id;
    result.device_index = device_index;
    result.protocol_major = ping->at(0);
    result.protocol_minor = ping->at(1);
    result.features = enumerate_feature_set(connection->get(), interface, device_index);
    const std::array<std::uint16_t, 11> checked_features = {0x0001, 0x0003, 0x0005, 0x1b04, 0x1c00, 0x2201, 0x2202, 0x8060, 0x8070, 0x8100, 0x8110};
    for (const std::uint16_t feature_id : checked_features) {
        const auto found = std::find_if(result.features.begin(), result.features.end(), [feature_id](const auto& feature) { return feature.id == feature_id; });
        if (found == result.features.end()) {
            const auto index = get_feature_index(connection->get(), interface, device_index, feature_id);
            result.features.push_back({feature_id, index.value_or(0), index.has_value() && index.value() != 0});
        }
    }
    const auto onboard_feature = std::find_if(result.features.begin(), result.features.end(), [](const auto& feature) { return feature.id == 0x8100 && feature.present; });
    if (onboard_feature != result.features.end()) {
        if (const auto snapshot = read_onboard_profiles(connection->get(), interface, device_index, onboard_feature->index)) {
            result.onboard_profiles = *snapshot;
        }
    }
    const auto lighting_feature = std::find_if(result.features.begin(), result.features.end(), [](const auto& feature) { return feature.id == 0x8070 && feature.present; });
    if (lighting_feature != result.features.end()) {
        if (const auto snapshot = read_lighting_state(connection->get(), interface, device_index, lighting_feature->index)) {
            result.lighting = *snapshot;
        }
    }
    if (const auto battery = read_battery_state(connection->get(), interface, device_index, result.features)) {
        result.battery = *battery;
    }
    const auto dpi_feature = std::find_if(result.features.begin(), result.features.end(), [](const auto& feature) { return feature.id == 0x2201 && feature.present; });
    if (dpi_feature != result.features.end()) {
        if (const auto dpi = read_dpi_state(connection->get(), interface, device_index, dpi_feature->index)) {
            result.dpi = *dpi;
        }
    }
    return result;
}

int bind_onboard_button_impl(std::uint8_t button, const std::array<std::uint8_t, 4>& spec) {
    const auto interfaces = logipro::enumerate_logitech_hid();
    for (const auto& interface : interfaces) {
        if (interface.product_id != receiver_product_id && interface.product_id != 0xc088 && interface.product_id != 0x4079) continue;
        const std::uint8_t first_index = interface.product_id == receiver_product_id ? 1 : direct_device_index;
        const std::uint8_t last_index = interface.product_id == receiver_product_id ? 6 : direct_device_index;
        for (std::uint8_t device_index = first_index; device_index <= last_index; ++device_index) {
            const auto device = probe_interface(interface, device_index);
            if (!device || !device->onboard_profiles.readable || !device->onboard_profiles.active_profile_readable || !device->onboard_profiles.active_profile_crc_valid) continue;
            const auto lighting_feature = std::find_if(device->features.begin(), device->features.end(), [](const auto& feature) { return feature.id == 0x8070 && feature.present; });
            const auto connection = open_connection(interface.path);
            if (!connection) continue;
            std::vector<std::uint8_t> profile;
            const auto old = read_button_spec(connection->get(), interface, device_index, device->onboard_profiles, button, profile);
            if (!old) {
                std::cerr << "Profile button must be 1-" << static_cast<unsigned>(device->onboard_profiles.button_count) << ".\n";
                return 2;
            }
            if (*old == spec) {
                std::cout << "Button " << static_cast<unsigned>(button) << " already matches." << '\n';
                return 0;
            }
            const auto path = backup_path(device->onboard_profiles.active_sector);
            if (!save_backup(path, profile, device->onboard_profiles.button_offset, device->onboard_profiles.button_count)) {
                std::cerr << "Refusing to overwrite invalid backup: " << path << '\n';
                return 1;
            }
            const std::size_t offset = device->onboard_profiles.button_offset + (button - 1) * 4;
            std::copy(spec.begin(), spec.end(), profile.begin() + offset);
            set_sector_crc(profile);
            if (!write_sector(connection->get(), interface, device_index, device->onboard_profiles.feature_index, device->onboard_profiles.active_sector, profile)) {
                std::cerr << "Profile write failed; original backup: " << path << '\n';
                return 1;
            }
            if (lighting_feature != device->features.end() && lighting_state_complete(device->lighting) && !restore_lighting_state(connection->get(), interface, device_index, device->lighting)) {
                std::cerr << "Profile write succeeded, but lighting-state restoration failed.\n";
                return 1;
            }
            const auto check = read_sector(connection->get(), interface, device_index, device->onboard_profiles.feature_index, device->onboard_profiles.active_sector, device->onboard_profiles.sector_size);
            std::array<std::uint8_t, 4> readback{};
            if (check && valid_sector_crc(*check) && offset + 4 <= check->size()) std::copy(check->begin() + offset, check->begin() + offset + 4, readback.begin());
            std::cout << "Button " << static_cast<unsigned>(button) << ":";
            print_spec(*old);
            std::cout << " ->";
            print_spec(spec);
            std::cout << '\n';
            if (!check || !valid_sector_crc(*check) || readback != spec) {
                std::cerr << "Readback verification failed; restore with --profile-restore.\n";
                return 1;
            }
            std::cout << "Verified. Backup: " << path << '\n';
            return 0;
        }
    }
    std::cerr << "No writable Logitech onboard-profile device found.\n";
    return 1;
}

int set_dpi_impl(std::uint8_t sensor, std::uint16_t dpi) {
    const auto interfaces = logipro::enumerate_logitech_hid();
    for (const auto& interface : interfaces) {
        if (interface.product_id != receiver_product_id && interface.product_id != 0xc088 && interface.product_id != 0x4079) continue;
        const std::uint8_t first_index = interface.product_id == receiver_product_id ? 1 : direct_device_index;
        const std::uint8_t last_index = interface.product_id == receiver_product_id ? 6 : direct_device_index;
        for (std::uint8_t device_index = first_index; device_index <= last_index; ++device_index) {
            const auto device = probe_interface(interface, device_index);
            if (!device || !device->dpi.readable || sensor >= device->dpi.sensors.size()) continue;
            const auto feature = std::find_if(device->features.begin(), device->features.end(), [](const auto& item) { return item.id == 0x2201 && item.present; });
            if (feature == device->features.end()) continue;
            const auto sensor_it = std::find_if(device->dpi.sensors.begin(), device->dpi.sensors.end(), [sensor](const auto& item) { return item.index == sensor; });
            if (sensor_it == device->dpi.sensors.end()) continue;
            const auto& sensor_info = *sensor_it;
            if (!dpi_supported(sensor_info, dpi)) continue;
            const auto connection = open_connection(interface.path);
            if (!connection || !set_dpi_state(connection->get(), interface, device_index, feature->index, sensor_info.index, dpi)) continue;
            const auto check = call(connection->get(), interface, device_index, feature->index, 0x20, {sensor_info.index, 0x00, 0x00});
            if (!check || check->size() < 3 || static_cast<std::uint16_t>(check->at(1) << 8 | check->at(2)) != dpi) continue;
            std::cout << "Sensor " << static_cast<unsigned>(sensor) << " DPI set to " << dpi << ".\n";
            return 0;
        }
    }
    std::cerr << "Unable to set live DPI.\n";
    return 1;
}

int set_onboard_dpi_impl(std::uint8_t slot, std::uint16_t dpi, bool set_default) {
    if (slot >= 5) return 2;
    const auto interfaces = logipro::enumerate_logitech_hid();
    for (const auto& interface : interfaces) {
        if (interface.product_id != receiver_product_id && interface.product_id != 0xc088 && interface.product_id != 0x4079) continue;
        const std::uint8_t first_index = interface.product_id == receiver_product_id ? 1 : direct_device_index;
        const std::uint8_t last_index = interface.product_id == receiver_product_id ? 6 : direct_device_index;
        for (std::uint8_t device_index = first_index; device_index <= last_index; ++device_index) {
            const auto device = probe_interface(interface, device_index);
            if (!device || !device->onboard_profiles.dpi_profile_readable || !device->onboard_profiles.active_profile_readable || !device->onboard_profiles.active_profile_crc_valid) continue;
            if (!set_default && slot >= device->onboard_profiles.dpi_profile_count) continue;
            const auto connection = open_connection(interface.path);
            if (!connection) continue;
            auto profile = read_sector(connection->get(), interface, device_index, device->onboard_profiles.feature_index, device->onboard_profiles.active_sector, device->onboard_profiles.sector_size);
            if (!profile || !valid_sector_crc(*profile) || profile->size() < 13) continue;
            if (!set_default && !device->dpi.sensors.empty() && !dpi_supported(device->dpi.sensors.front(), dpi)) continue;
            const std::size_t dpi_offset = 3 + static_cast<std::size_t>(slot) * 2;
            if (set_default) {
                if (profile->at(1) == slot) {
                    std::cout << "DPI slot " << static_cast<unsigned>(slot + 1) << " is already the default.\n";
                    return 0;
                }
            } else if (static_cast<std::uint16_t>(profile->at(dpi_offset) | profile->at(dpi_offset + 1) << 8) == dpi) {
                std::cout << "DPI slot " << static_cast<unsigned>(slot + 1) << " already matches " << dpi << ".\n";
                return 0;
            }
            const auto path = backup_path(device->onboard_profiles.active_sector);
            std::vector<std::pair<std::size_t, std::size_t>> mutable_ranges = {{device->onboard_profiles.button_offset, device->onboard_profiles.button_offset + static_cast<std::size_t>(device->onboard_profiles.button_count) * 4}, {1, 13}};
            if (device->onboard_profiles.profile_format == 3 && profile->size() >= 252) mutable_ranges.push_back({208, 252});
            if (!save_backup(path, *profile, mutable_ranges)) {
                std::cerr << "Refusing to overwrite invalid backup: " << path << '\n';
                return 1;
            }
            if (set_default) {
                profile->at(1) = slot;
            } else {
                profile->at(dpi_offset) = static_cast<std::uint8_t>(dpi);
                profile->at(dpi_offset + 1) = static_cast<std::uint8_t>(dpi >> 8);
            }
            set_sector_crc(*profile);
            if (!write_sector(connection->get(), interface, device_index, device->onboard_profiles.feature_index, device->onboard_profiles.active_sector, *profile)) {
                std::cerr << "DPI profile write failed; original backup: " << path << '\n';
                return 1;
            }
            const auto lighting_feature = std::find_if(device->features.begin(), device->features.end(), [](const auto& item) { return item.id == 0x8070 && item.present; });
            if (lighting_feature != device->features.end() && lighting_state_complete(device->lighting) && !restore_lighting_state(connection->get(), interface, device_index, device->lighting)) {
                std::cerr << "DPI profile write succeeded, but lighting-state restoration failed.\n";
                return 1;
            }
            const auto check = read_sector(connection->get(), interface, device_index, device->onboard_profiles.feature_index, device->onboard_profiles.active_sector, profile->size());
            const bool verified = check && valid_sector_crc(*check) && (set_default ? check->at(1) == slot : static_cast<std::uint16_t>(check->at(dpi_offset) | check->at(dpi_offset + 1) << 8) == dpi);
            if (!verified) {
                std::cerr << "DPI profile readback verification failed; restore with --profile-restore.\n";
                return 1;
            }
            std::cout << (set_default ? "Default DPI slot set to " : "DPI slot ") << static_cast<unsigned>(slot + 1) << (set_default ? ".\n" : " -> " + std::to_string(dpi) + ".\n");
            return 0;
        }
    }
    std::cerr << "No writable onboard DPI profile found.\n";
    return 1;
}

int restore_onboard_profile_impl() {
    const auto interfaces = logipro::enumerate_logitech_hid();
    for (const auto& interface : interfaces) {
        if (interface.product_id != receiver_product_id && interface.product_id != 0xc088 && interface.product_id != 0x4079) continue;
        const std::uint8_t first_index = interface.product_id == receiver_product_id ? 1 : direct_device_index;
        const std::uint8_t last_index = interface.product_id == receiver_product_id ? 6 : direct_device_index;
        for (std::uint8_t device_index = first_index; device_index <= last_index; ++device_index) {
            const auto device = probe_interface(interface, device_index);
            if (!device || !device->onboard_profiles.readable || !device->onboard_profiles.active_profile_readable) continue;
            const auto lighting_feature = std::find_if(device->features.begin(), device->features.end(), [](const auto& feature) { return feature.id == 0x8070 && feature.present; });
            const auto path = backup_path(device->onboard_profiles.active_sector);
            std::ifstream input(path, std::ios::binary);
            if (!input) continue;
            const std::vector<std::uint8_t> backup((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            if (backup.size() != device->onboard_profiles.sector_size || !valid_sector_crc(backup)) {
                std::cerr << "Invalid backup: " << path << '\n';
                return 1;
            }
            const auto connection = open_connection(interface.path);
            if (!connection) {
                std::cerr << "Profile restore failed.\n";
                return 1;
            }
            auto restored = backup;
            const auto current = read_sector(connection->get(), interface, device_index, device->onboard_profiles.feature_index, device->onboard_profiles.active_sector, device->onboard_profiles.sector_size);
            bool preserved_lighting = false;
            if (device->onboard_profiles.profile_format == 3 && (!current || !valid_sector_crc(*current) || current->size() < 252 || restored.size() < 252)) {
                std::cerr << "Cannot restore profile without reading current lighting state.\n";
                return 1;
            }
            if (current && valid_sector_crc(*current) && device->onboard_profiles.profile_format == 3 && current->size() >= 252 && restored.size() >= 252) {
                std::copy(current->begin() + 208, current->begin() + 252, restored.begin() + 208);
                set_sector_crc(restored);
                preserved_lighting = true;
            }
            if (!write_sector(connection->get(), interface, device_index, device->onboard_profiles.feature_index, device->onboard_profiles.active_sector, restored)) {
                std::cerr << "Profile restore failed.\n";
                return 1;
            }
            if (lighting_feature != device->features.end() && lighting_state_complete(device->lighting) && !restore_lighting_state(connection->get(), interface, device_index, device->lighting)) {
                std::cerr << "Profile restore succeeded, but lighting-state restoration failed.\n";
                return 1;
            }
            const auto check = read_sector(connection->get(), interface, device_index, device->onboard_profiles.feature_index, device->onboard_profiles.active_sector, restored.size());
            if (!check || *check != restored || !valid_sector_crc(*check)) {
                std::cerr << "Profile restore verification failed.\n";
                return 1;
            }
            std::cout << "Restored sector " << device->onboard_profiles.active_sector << " from " << path << (preserved_lighting ? " (preserved current lighting)" : "") << '\n';
            return 0;
        }
    }
    std::cerr << "No matching onboard-profile backup found.\n";
    return 1;
}

int disable_onboard_lighting_impl() {
    const auto interfaces = logipro::enumerate_logitech_hid();
    for (const auto& interface : interfaces) {
        if (interface.product_id != receiver_product_id && interface.product_id != 0xc088 && interface.product_id != 0x4079) continue;
        const std::uint8_t first_index = interface.product_id == receiver_product_id ? 1 : direct_device_index;
        const std::uint8_t last_index = interface.product_id == receiver_product_id ? 6 : direct_device_index;
        for (std::uint8_t device_index = first_index; device_index <= last_index; ++device_index) {
            const auto device = probe_interface(interface, device_index);
            if (!device || !device->onboard_profiles.readable || !device->onboard_profiles.active_profile_readable || !device->onboard_profiles.active_profile_crc_valid || device->onboard_profiles.profile_format != 3 || !device->onboard_profiles.active_lighting_readable) continue;
            const auto lighting_feature = std::find_if(device->features.begin(), device->features.end(), [](const auto& feature) { return feature.id == 0x8070 && feature.present; });
            if (lighting_feature == device->features.end() || device->lighting.zone_count == 0 || device->lighting.zone_count > 4) continue;
            const auto connection = open_connection(interface.path);
            if (!connection) continue;
            const auto path = backup_path(device->onboard_profiles.active_sector);
            auto profile = read_sector(connection->get(), interface, device_index, device->onboard_profiles.feature_index, device->onboard_profiles.active_sector, device->onboard_profiles.sector_size);
            if (!profile || !valid_sector_crc(*profile) || !save_backup(path, *profile, device->onboard_profiles.button_offset, device->onboard_profiles.button_count)) {
                std::cerr << "Refusing to overwrite invalid backup: " << path << '\n';
                return 1;
            }
            for (std::size_t index = 0; index < device->lighting.zone_count; ++index) std::fill(profile->begin() + 208 + index * 11, profile->begin() + 208 + (index + 1) * 11, 0);
            set_sector_crc(*profile);
            if (!write_sector(connection->get(), interface, device_index, device->onboard_profiles.feature_index, device->onboard_profiles.active_sector, *profile)) {
                std::cerr << "Lighting profile write failed; original backup: " << path << '\n';
                return 1;
            }
            if (!set_lighting_effect(connection->get(), interface, device_index, device->lighting, 0)) {
                std::cerr << "Lighting profile updated, but live LED disable failed.\n";
                return 1;
            }
            const auto check = read_sector(connection->get(), interface, device_index, device->onboard_profiles.feature_index, device->onboard_profiles.active_sector, profile->size());
            bool verified = check && valid_sector_crc(*check);
            for (std::size_t index = 0; verified && index < device->lighting.zone_count; ++index) verified = check->at(208 + index * 11) == 0;
            if (!verified) {
                std::cerr << "Lighting profile readback verification failed; restore with --profile-restore.\n";
                return 1;
            }
            std::cout << "Onboard lighting disabled. Backup: " << path << '\n';
            return 0;
        }
    }
    std::cerr << "No writable Logitech onboard-lighting profile found.\n";
    return 1;
}

}

namespace logipro {

std::vector<HidppDeviceInfo> probe_logitech_hidpp(const std::vector<HidDeviceInfo>& interfaces) {
    std::vector<HidppDeviceInfo> devices;
    for (const auto& interface : interfaces) {
        if (interface.product_id == receiver_product_id) {
            for (std::uint8_t index = 1; index <= 6; ++index) {
                if (const auto device = probe_interface(interface, index)) {
                    devices.push_back(*device);
                }
            }
            continue;
        }
        if (interface.product_id == 0xc088 || interface.product_id == 0x4079) {
            if (const auto device = probe_interface(interface, direct_device_index)) {
                devices.push_back(*device);
            }
        }
    }
    return devices;
}

int bind_onboard_button(std::uint8_t button, const std::array<std::uint8_t, 4>& spec) {
    ScopedOutputSilencer silencer;
    return bind_onboard_button_impl(button, spec);
}

int set_dpi(std::uint8_t sensor, std::uint16_t dpi) {
    ScopedOutputSilencer silencer;
    return set_dpi_impl(sensor, dpi);
}

int set_onboard_dpi(std::uint8_t slot, std::uint16_t dpi) {
    ScopedOutputSilencer silencer;
    return set_onboard_dpi_impl(slot, dpi, false);
}

int set_onboard_default_dpi(std::uint8_t slot) {
    ScopedOutputSilencer silencer;
    return set_onboard_dpi_impl(slot, 0, true);
}

int restore_onboard_profile() {
    ScopedOutputSilencer silencer;
    return restore_onboard_profile_impl();
}

int set_lighting_effect(std::uint16_t effect_id) {
    return set_lighting_effect_settings(effect_id, 8000, 100);
}

int set_lighting_effect_settings(std::uint16_t effect_id, std::uint16_t period_ms, std::uint8_t brightness) {
    ScopedOutputSilencer silencer;
    const auto interfaces = logipro::enumerate_logitech_hid();
    for (const auto& interface : interfaces) {
        if (interface.product_id != receiver_product_id && interface.product_id != 0xc088 && interface.product_id != 0x4079) continue;
        const std::uint8_t first_index = interface.product_id == receiver_product_id ? 1 : direct_device_index;
        const std::uint8_t last_index = interface.product_id == receiver_product_id ? 6 : direct_device_index;
        for (std::uint8_t device_index = first_index; device_index <= last_index; ++device_index) {
            const auto device = probe_interface(interface, device_index);
            if (!device || !device->lighting.readable || !device->lighting.software_control_readable || device->lighting.zone_count == 0) continue;
            const auto connection = open_connection(interface.path);
            if (connection && set_lighting_effect_state(connection->get(), interface, device_index, device->lighting, effect_id, period_ms, brightness)) return 0;
        }
    }
    return 1;
}

int set_lighting_software_control(bool enabled) {
    ScopedOutputSilencer silencer;
    const auto interfaces = logipro::enumerate_logitech_hid();
    for (const auto& interface : interfaces) {
        if (interface.product_id != receiver_product_id && interface.product_id != 0xc088 && interface.product_id != 0x4079) continue;
        const std::uint8_t first_index = interface.product_id == receiver_product_id ? 1 : direct_device_index;
        const std::uint8_t last_index = interface.product_id == receiver_product_id ? 6 : direct_device_index;
        for (std::uint8_t device_index = first_index; device_index <= last_index; ++device_index) {
            const auto device = probe_interface(interface, device_index);
            if (!device || !device->lighting.readable || !device->lighting.software_control_readable || device->lighting.zone_count == 0) continue;
            const auto connection = open_connection(interface.path);
            if (connection && set_lighting_software_control_state(connection->get(), interface, device_index, device->lighting, enabled)) return 0;
        }
    }
    return 1;
}

int disable_onboard_lighting() {
    ScopedOutputSilencer silencer;
    return disable_onboard_lighting_impl();
}

}

#else

namespace logipro {

std::vector<HidppDeviceInfo> probe_logitech_hidpp(const std::vector<HidDeviceInfo>&) {
    return {};
}

int bind_onboard_button(std::uint8_t, const std::array<std::uint8_t, 4>&) {
    return 1;
}

int set_dpi(std::uint8_t, std::uint16_t) {
    return 1;
}

int set_onboard_dpi(std::uint8_t, std::uint16_t) {
    return 1;
}

int set_onboard_default_dpi(std::uint8_t) {
    return 1;
}

int restore_onboard_profile() {
    return 1;
}

int set_lighting_effect(std::uint16_t) {
    return 1;
}

int set_lighting_effect_settings(std::uint16_t, std::uint16_t, std::uint8_t) {
    return 1;
}

int set_lighting_software_control(bool) {
    return 1;
}

int disable_onboard_lighting() {
    return 1;
}

}

#endif
