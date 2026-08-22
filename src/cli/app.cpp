#include "logipro/api.h"
#include "logipro/app.hpp"

#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view version = "0.1.0";

struct SnapshotGuard {
    logipro_snapshot_t* value = nullptr;
    ~SnapshotGuard() { logipro_snapshot_destroy(value); }
};

void print_help() {
    std::cout << "LogiPro " << version << "\n"
              << "Logitech G Pro Wireless control application.\n\n"
              << "Usage: logipro [--help] [--version] [--list-hid] [--probe-hidpp]\n"
              << "       logipro [--capture-hid] [--watch-buttons] [--debug]\n"
              << "       logipro [--bind-left KEY] [--bind-right KEY] [--bind-middle KEY]\n"
              << "               [--bind-back KEY] [--bind-forward KEY]\n"
              << "       logipro [--profile-bind BUTTON KEY] [--profile-restore]\n"
              << "               [--profile-lighting-off]\n\n"
              << "With no command flag, the GTK interface opens. --debug enables library diagnostics.\n"
              << "Host keys: A-Z, 0-9, F1-F24, SPACE, ENTER, TAB, ESC, BACKSPACE, SHIFT, CTRL, ALT\n"
              << "Profile keys: A-Z, 0-9, F1-F24, SPACE, ENTER, TAB, ESC, BACKSPACE, or raw 8-hex spec\n";
}

std::optional<std::uint16_t> parse_key(std::string_view value) {
    std::string key(value);
    for (char& character : key) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    if (key.size() == 1 && ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= '0' && key[0] <= '9'))) return static_cast<std::uint16_t>(key[0]);
    if (key.size() > 1 && key[0] == 'F') {
        int function_key = 0;
        const auto [end, error] = std::from_chars(key.data() + 1, key.data() + key.size(), function_key);
        if (error == std::errc{} && end == key.data() + key.size() && function_key >= 1 && function_key <= 24) return static_cast<std::uint16_t>(0x6f + function_key);
    }
    if (key == "SPACE") return 0x20;
    if (key == "ENTER") return 0x0d;
    if (key == "TAB") return 0x09;
    if (key == "ESC" || key == "ESCAPE") return 0x1b;
    if (key == "BACKSPACE") return 0x08;
    if (key == "SHIFT") return 0x10;
    if (key == "CTRL" || key == "CONTROL") return 0x11;
    if (key == "ALT") return 0x12;
    return std::nullopt;
}

std::optional<std::uint8_t> parse_hid_key(std::string_view value) {
    std::string key(value);
    for (char& character : key) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z') return static_cast<std::uint8_t>(0x04 + key[0] - 'A');
    if (key.size() == 1 && key[0] >= '1' && key[0] <= '9') return static_cast<std::uint8_t>(0x1e + key[0] - '1');
    if (key == "0") return 0x27;
    if (key.size() > 1 && key[0] == 'F') {
        int function_key = 0;
        const auto [end, error] = std::from_chars(key.data() + 1, key.data() + key.size(), function_key);
        if (error == std::errc{} && end == key.data() + key.size() && function_key >= 1 && function_key <= 24) return static_cast<std::uint8_t>(function_key <= 12 ? 0x39 + function_key : 0x68 + function_key - 13);
    }
    if (key == "ENTER") return 0x28;
    if (key == "ESC" || key == "ESCAPE") return 0x29;
    if (key == "BACKSPACE") return 0x2a;
    if (key == "TAB") return 0x2b;
    if (key == "SPACE") return 0x2c;
    return std::nullopt;
}

std::optional<std::array<std::uint8_t, 4>> parse_profile_target(std::string_view value) {
    std::string target(value);
    for (char& character : target) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    const std::array<std::pair<std::string_view, std::uint16_t>, 5> mouse_buttons = {{{"LEFT", 0x0001}, {"RIGHT", 0x0002}, {"MIDDLE", 0x0004}, {"BACK", 0x0008}, {"FORWARD", 0x0010}}};
    for (const auto& [name, mask] : mouse_buttons) if (target == name) return std::array<std::uint8_t, 4>{0x80, 0x01, static_cast<std::uint8_t>(mask >> 8), static_cast<std::uint8_t>(mask)};
    if (target == "DPI-CYCLE" || target == "CYCLE-DPI") return std::array<std::uint8_t, 4>{0x90, 0x05, 0x00, 0x00};
    if (const auto key = parse_hid_key(target)) return std::array<std::uint8_t, 4>{0x80, 0x02, 0x00, *key};
    if (target.size() == 8) {
        std::array<std::uint8_t, 4> spec{};
        for (std::size_t index = 0; index < spec.size(); ++index) {
            unsigned int byte = 0;
            const auto [end, error] = std::from_chars(target.data() + index * 2, target.data() + index * 2 + 2, byte, 16);
            if (error != std::errc{} || end != target.data() + index * 2 + 2) return std::nullopt;
            spec[index] = static_cast<std::uint8_t>(byte);
        }
        return spec;
    }
    return std::nullopt;
}

int snapshot_or_report(SnapshotGuard& snapshot) {
    const int result = logipro_snapshot_create(&snapshot.value);
    if (result != LOGIPRO_OK) std::cerr << (logipro_last_error()[0] == '\0' ? "Unable to read Logitech devices." : logipro_last_error()) << '\n';
    return result;
}

void print_hid_devices() {
    SnapshotGuard snapshot;
    if (snapshot_or_report(snapshot) != LOGIPRO_OK) return;
    const auto count = logipro_snapshot_hid_count(snapshot.value);
    std::cout << "Logitech HID interfaces: " << count << '\n';
    for (std::size_t index = 0; index < count; ++index) {
        logipro_hid_info_t device{};
        if (logipro_snapshot_get_hid(snapshot.value, index, &device) != LOGIPRO_OK) continue;
        std::cout << "  " << device.product_id << "  " << (device.product == nullptr || device.product[0] == '\0' ? "(unnamed)" : device.product) << '\n'
                  << "    usage: 0x" << std::hex << std::setw(4) << std::setfill('0') << device.usage_page
                  << ":0x" << std::setw(4) << device.usage << std::dec << std::setfill(' ') << '\n'
                  << "    reports: input=" << device.input_report_length << " output=" << device.output_report_length << " feature=" << device.feature_report_length << '\n'
                  << "    path: " << (device.path == nullptr ? "" : device.path) << '\n';
    }
}

void print_hidpp_devices() {
    SnapshotGuard snapshot;
    if (snapshot_or_report(snapshot) != LOGIPRO_OK) return;
    const auto count = logipro_snapshot_device_count(snapshot.value);
    std::cout << "HID++ devices: " << count << '\n';
    for (std::size_t index = 0; index < count; ++index) {
        logipro_device_info_t device{};
        if (logipro_snapshot_get_device(snapshot.value, index, &device) != LOGIPRO_OK) continue;
        std::cout << "  " << (device.product == nullptr || device.product[0] == '\0' ? "(unnamed)" : device.product)
                  << " index=0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(device.device_index)
                  << " protocol=" << std::dec << static_cast<unsigned>(device.protocol_major) << '.' << static_cast<unsigned>(device.protocol_minor) << std::setfill(' ') << '\n';
        for (std::size_t feature_index = 0; feature_index < device.feature_count; ++feature_index) {
            logipro_feature_info_t feature{};
            if (logipro_snapshot_get_feature(snapshot.value, index, feature_index, &feature) != LOGIPRO_OK) continue;
            std::cout << "    feature 0x" << std::hex << std::setw(4) << std::setfill('0') << feature.id << (feature.present ? " index=0x" : " absent");
            if (feature.present) std::cout << std::setw(2) << static_cast<unsigned>(feature.index) << " version=" << std::dec << static_cast<unsigned>(feature.version) << " type=0x" << std::hex << std::setw(2) << static_cast<unsigned>(feature.type);
            std::cout << std::dec << std::setfill(' ') << '\n';
        }
        if (device.onboard_profiles_readable) {
            std::cout << "  onboard profiles index=0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(device.onboard_feature_index)
                      << " format=" << std::dec << static_cast<unsigned>(device.profile_format) << " buttons=" << static_cast<unsigned>(device.button_count) << " sector-size=" << device.sector_size << '\n';
            if (device.active_profile_readable) {
                std::cout << "    active sector=0x" << std::hex << std::setw(4) << std::setfill('0') << device.active_sector
                          << (device.active_profile_crc_valid ? " crc=ok" : " crc=bad") << std::dec << " button-offset=" << static_cast<unsigned>(device.button_offset) << '\n';
                for (std::uint8_t button = 1; button <= device.button_count; ++button) {
                    std::array<std::uint8_t, 4> spec{};
                    if (logipro_snapshot_get_button(snapshot.value, index, button, spec.data()) != LOGIPRO_OK) continue;
                    std::cout << "      button " << static_cast<unsigned>(button) << ':';
                    for (const auto byte : spec) std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
                    std::cout << std::dec << std::setfill(' ') << '\n';
                }
                for (std::size_t lighting_index = 0; lighting_index < device.active_lighting_count; ++lighting_index) {
                    std::array<std::uint8_t, 11> record{};
                    if (logipro_snapshot_get_active_lighting(snapshot.value, index, lighting_index, record.data()) != LOGIPRO_OK) continue;
                    std::cout << "      lighting " << lighting_index + 1 << " bytes=";
                    for (const auto byte : record) std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
                    std::cout << std::dec << std::setfill(' ') << '\n';
                }
            }
        }
        if (device.lighting_readable) {
            std::cout << "  lighting index=0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(device.lighting_feature_index)
                      << " zones=" << std::dec << static_cast<unsigned>(device.lighting_declared_zone_count)
                      << " nv=0x" << std::hex << std::setw(4) << device.lighting_nv_capabilities
                      << " ext=0x" << std::setw(4) << device.lighting_extended_capabilities << " captured=" << std::dec << device.lighting_zone_records;
            if (device.lighting_software_control_readable) std::cout << " control=" << (device.lighting_software_control == 0 ? "firmware" : "software") << " sync=" << static_cast<unsigned>(device.lighting_sync_events);
            std::cout << '\n';
            for (std::size_t zone_index = 0; zone_index < device.lighting_zone_records; ++zone_index) {
                logipro_lighting_zone_info_t zone{};
                if (logipro_snapshot_get_lighting_zone(snapshot.value, index, zone_index, &zone) != LOGIPRO_OK) continue;
                std::cout << "    zone request=" << static_cast<unsigned>(zone.requested_zone) << " info=" << static_cast<unsigned>(zone.info_zone)
                          << " location=0x" << std::hex << std::setw(4) << std::setfill('0') << zone.location << std::dec << " effects=" << static_cast<unsigned>(zone.effect_count)
                          << " effect=0x" << std::hex << std::setw(2) << static_cast<unsigned>(zone.effect) << std::dec << std::setfill(' ') << '\n';
                if (zone.effect_count > 0) {
                    std::cout << "      effect ids=";
                    for (std::size_t effect_index = 0; effect_index < zone.effect_count; ++effect_index) {
                        std::uint16_t effect_id = 0;
                        if (logipro_snapshot_get_lighting_effect_id(snapshot.value, index, zone_index, effect_index, &effect_id) != LOGIPRO_OK) continue;
                        if (effect_index != 0) std::cout << ',';
                        std::cout << "0x" << std::hex << std::setw(2) << std::setfill('0') << effect_id;
                    }
                    std::cout << std::dec << std::setfill(' ') << '\n';
                }
            }
        }
        std::cout << "    path: " << (device.path == nullptr ? "" : device.path) << '\n';
    }
}

int report_operation(int result) {
    if (result != LOGIPRO_OK) std::cerr << (logipro_last_error()[0] == '\0' ? "LogiPro operation failed." : logipro_last_error()) << '\n';
    return result;
}

}

namespace logipro {

bool cli_requested(int argc, char* const argv[]) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument != "--debug" && argument != "--gui") return true;
    }
    return false;
}

int run_cli(int argc, char* argv[]) {
    bool debug = false;
    for (int index = 1; index < argc; ++index) if (std::string_view(argv[index]) == "--debug") debug = true;
    logipro_debug_set_enabled(debug ? 1 : 0);
    std::vector<logipro_mouse_binding_t> bindings;
    struct ProfileBinding { std::uint8_t button; std::array<std::uint8_t, 4> spec; };
    std::vector<ProfileBinding> profile_bindings;
    bool watch_buttons = false;
    bool capture_hid = false;
    bool profile_restore = false;
    bool profile_lighting_off = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--debug" || argument == "--gui") continue;
        if (argument == "--help" || argument == "-h") {
            print_help();
            return 0;
        }
        if (argument == "--version" || argument == "-v") {
            std::cout << version << '\n';
            return 0;
        }
        if (argument == "--list-hid") {
            print_hid_devices();
            return 0;
        }
        if (argument == "--probe-hidpp") {
            print_hidpp_devices();
            return 0;
        }
        if (argument == "--watch-buttons") {
            watch_buttons = true;
            continue;
        }
        if (argument == "--capture-hid") {
            capture_hid = true;
            continue;
        }
        if (argument == "--profile-restore") {
            profile_restore = true;
            continue;
        }
        if (argument == "--profile-lighting-off") {
            profile_lighting_off = true;
            continue;
        }
        if (argument == "--profile-bind") {
            if (index + 2 >= argc) {
                std::cerr << argument << " requires BUTTON and KEY.\n";
                return 2;
            }
            unsigned int button = 0;
            const char* button_text = argv[++index];
            const auto button_end = button_text + std::strlen(button_text);
            const auto [end, error] = std::from_chars(button_text, button_end, button);
            if (error != std::errc{} || end != button_end || button < 1 || button > 8) {
                std::cerr << "Profile button must be 1-8.\n";
                return 2;
            }
            const auto spec = parse_profile_target(argv[++index]);
            if (!spec) {
                std::cerr << "Unsupported profile key: " << argv[index] << '\n';
                return 2;
            }
            profile_bindings.push_back({static_cast<std::uint8_t>(button), *spec});
            continue;
        }
        if (argument == "--bind-left" || argument == "--bind-right" || argument == "--bind-middle" || argument == "--bind-back" || argument == "--bind-forward") {
            if (index + 1 >= argc) {
                std::cerr << argument << " requires a key.\n";
                return 2;
            }
            const auto key = parse_key(argv[++index]);
            if (!key) {
                std::cerr << "Unsupported key: " << argv[index] << '\n';
                return 2;
            }
            std::uint8_t button = LOGIPRO_MOUSE_FORWARD;
            if (argument == "--bind-left") button = LOGIPRO_MOUSE_LEFT;
            else if (argument == "--bind-right") button = LOGIPRO_MOUSE_RIGHT;
            else if (argument == "--bind-middle") button = LOGIPRO_MOUSE_MIDDLE;
            else if (argument == "--bind-back") button = LOGIPRO_MOUSE_BACK;
            bindings.push_back({button, *key});
            continue;
        }
        std::cerr << "Unknown argument: " << argument << '\n';
        return 2;
    }
    if (capture_hid) return report_operation(logipro_capture_raw_input());
    if (!bindings.empty()) return report_operation(logipro_run_mouse_bindings(bindings.data(), bindings.size()));
    if (!profile_bindings.empty()) {
        for (const auto& binding : profile_bindings) {
            const int result = logipro_profile_bind(binding.button, binding.spec.data());
            if (result != LOGIPRO_OK) return report_operation(result);
            std::cout << "Button " << static_cast<unsigned>(binding.button) << " updated.\n";
        }
        return 0;
    }
    if (profile_restore) {
        const int result = logipro_profile_restore();
        if (result == LOGIPRO_OK) std::cout << "Profile restored.\n";
        return report_operation(result);
    }
    if (profile_lighting_off) {
        const int result = logipro_profile_lighting_off();
        if (result == LOGIPRO_OK) std::cout << "Onboard lighting disabled.\n";
        return report_operation(result);
    }
    if (watch_buttons) return report_operation(logipro_watch_buttons());
    print_help();
    return 0;
}

}
