#include "logipro/app.hpp"
#include "logipro/hid.hpp"
#include "logipro/hid_capture.hpp"
#include "logipro/hidpp.hpp"
#include "logipro/hook.hpp"

#include <array>
#include <charconv>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view version = "0.1.0";

void print_help() {
    std::cout << "LogiPro " << version << "\n"
              << "Standalone Logitech G Pro Wireless configuration tool.\n\n"
              << "Usage: logipro [--help] [--version] [--list-hid] [--probe-hidpp] [--capture-hid] [--watch-buttons]\n"
              << "       logipro [--bind-left KEY] [--bind-right KEY] [--bind-middle KEY]\n"
              << "               [--bind-back KEY] [--bind-forward KEY] [--profile-bind BUTTON KEY]\n"
              << "               [--profile-restore] [--profile-lighting-off]\n"
              << "Host keys: A-Z, 0-9, F1-F24, SPACE, ENTER, TAB, ESC, BACKSPACE, SHIFT, CTRL, ALT\n"
              << "Profile keys: A-Z, 0-9, F1-F24, SPACE, ENTER, TAB, ESC, BACKSPACE, or raw 8-hex spec\n";
}

std::optional<std::uint16_t> parse_key(std::string_view value) {
    std::string key(value);
    for (char& character : key) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    if (key.size() == 1 && ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= '0' && key[0] <= '9'))) {
        return static_cast<std::uint16_t>(key[0]);
    }
    if (key.size() > 1 && key[0] == 'F') {
        int function_key = 0;
        const auto [end, error] = std::from_chars(key.data() + 1, key.data() + key.size(), function_key);
        if (error == std::errc{} && end == key.data() + key.size() && function_key >= 1 && function_key <= 24) {
            return static_cast<std::uint16_t>(0x6f + function_key);
        }
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
    for (char& character : key) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z') return static_cast<std::uint8_t>(0x04 + key[0] - 'A');
    if (key.size() == 1 && key[0] >= '1' && key[0] <= '9') return static_cast<std::uint8_t>(0x1e + key[0] - '1');
    if (key == "0") return 0x27;
    if (key.size() > 1 && key[0] == 'F') {
        int function_key = 0;
        const auto [end, error] = std::from_chars(key.data() + 1, key.data() + key.size(), function_key);
        if (error == std::errc{} && end == key.data() + key.size() && function_key >= 1 && function_key <= 24) {
            return static_cast<std::uint8_t>(function_key <= 12 ? 0x39 + function_key : 0x68 + function_key - 13);
        }
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
    for (char& character : target) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    const std::array<std::pair<std::string_view, std::uint16_t>, 5> mouse_buttons = {{{"LEFT", 0x0001}, {"RIGHT", 0x0002}, {"MIDDLE", 0x0004}, {"BACK", 0x0008}, {"FORWARD", 0x0010}}};
    for (const auto& [name, mask] : mouse_buttons) {
        if (target == name) return std::array<std::uint8_t, 4>{0x80, 0x01, static_cast<std::uint8_t>(mask >> 8), static_cast<std::uint8_t>(mask)};
    }
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

void print_hidpp_devices() {
#ifdef _WIN32
    const auto devices = logipro::probe_logitech_hidpp(logipro::enumerate_logitech_hid());
    std::wcout << L"HID++ devices: " << devices.size() << L'\n';
    for (const auto& device : devices) {
        std::wcout << L"  " << (device.product.empty() ? L"(unnamed)" : device.product)
                   << L" index=0x" << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(device.device_index)
                   << L" protocol=" << std::dec << static_cast<unsigned>(device.protocol_major)
                   << L'.' << static_cast<unsigned>(device.protocol_minor) << std::setfill(L' ') << L'\n';
        for (const auto& feature : device.features) {
            std::wcout << L"    feature 0x" << std::hex << std::setw(4) << std::setfill(L'0') << feature.id
                       << (feature.present ? L" index=0x" : L" absent") << std::setfill(L'0');
            if (feature.present) {
                std::wcout << std::setw(2) << static_cast<unsigned>(feature.index)
                           << L" version=" << std::dec << static_cast<unsigned>(feature.version)
                           << L" type=0x" << std::hex << std::setw(2) << static_cast<unsigned>(feature.type);
            }
            std::wcout << std::dec << std::setfill(L' ') << L'\n';
        }
        if (device.onboard_profiles.readable) {
            const auto& onboard = device.onboard_profiles;
            std::wcout << L"  onboard profiles index=0x" << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(onboard.feature_index);
            if (onboard.mode_readable) {
                std::wcout << L" mode=0x" << std::setw(2) << static_cast<unsigned>(onboard.mode);
            }
            std::wcout << L" format=" << std::dec << static_cast<unsigned>(onboard.profile_format)
                       << L" buttons=" << static_cast<unsigned>(onboard.button_count)
                       << L" sector-size=" << onboard.sector_size << L'\n';
            for (const auto& profile : onboard.profiles) {
                std::wcout << L"    profile sector=0x" << std::hex << std::setw(4) << std::setfill(L'0') << profile.sector
                           << L" enabled=0x" << std::setw(2) << static_cast<unsigned>(profile.enabled) << std::dec << std::setfill(L' ') << L'\n';
            }
            if (onboard.active_profile_readable) {
                std::wcout << L"    active sector=0x" << std::hex << std::setw(4) << std::setfill(L'0') << onboard.active_sector
                           << (onboard.active_profile_crc_valid ? L" crc=ok" : L" crc=bad")
                           << std::dec << L" button-offset=" << static_cast<unsigned>(onboard.button_offset) << L'\n';
                for (std::size_t index = 0; index < onboard.buttons.size(); ++index) {
                    const auto& spec = onboard.buttons[index].spec;
                    std::wcout << L"      button " << index + 1 << L":";
                    for (const auto byte : spec) {
                        std::wcout << L" " << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(byte);
                    }
                    std::wcout << std::dec << std::setfill(L' ') << L'\n';
                }
                for (std::size_t index = 0; index < onboard.active_lighting.size(); ++index) {
                    std::wcout << L"      lighting " << index + 1 << L" id=0x" << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(onboard.active_lighting[index][0]) << L" bytes=";
                    for (const auto byte : onboard.active_lighting[index]) std::wcout << std::setw(2) << static_cast<unsigned>(byte) << L' ';
                    std::wcout << std::dec << std::setfill(L' ') << L'\n';
                }
            }
        }
        if (device.lighting.readable) {
            std::wcout << L"  lighting index=0x" << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(device.lighting.feature_index)
                       << L" zones=" << std::dec << static_cast<unsigned>(device.lighting.zone_count)
                       << L" nv=0x" << std::hex << std::setw(4) << static_cast<unsigned>(device.lighting.nv_capabilities)
                       << L" ext=0x" << std::setw(4) << static_cast<unsigned>(device.lighting.extended_capabilities)
                       << L" captured=" << device.lighting.zones.size();
            if (device.lighting.software_control_readable) {
                std::wcout << L" control=" << (device.lighting.software_control == 0 ? L"firmware" : L"software")
                           << L" sync=" << static_cast<unsigned>(device.lighting.sync_events);
            }
            std::wcout << L'\n';
            for (const auto& zone : device.lighting.zones) {
                std::wcout << L"    zone request=" << static_cast<unsigned>(zone.requested_zone) << L" info=" << static_cast<unsigned>(zone.info_zone)
                           << L" location=0x" << std::hex << std::setw(4) << static_cast<unsigned>(zone.location) << std::dec
                           << L" effects=" << static_cast<unsigned>(zone.effect_count);
                if (!zone.effect_ids.empty()) {
                    std::wcout << L" ids=";
                    for (const auto effect_id : zone.effect_ids) std::wcout << L"0x" << std::hex << std::setw(4) << std::setfill(L'0') << effect_id << L' ';
                    std::wcout << std::dec << std::setfill(L' ');
                }
                if (zone.effect_readable) {
                    std::wcout << L" reply=" << static_cast<unsigned>(zone.effect_zone)
                               << L" effect=0x" << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(zone.effect)
                               << std::dec << std::setfill(L' ');
                }
                if (zone.settings_readable) {
                    std::wcout << L" settings-zone=" << static_cast<unsigned>(zone.settings_zone) << L" settings=";
                    for (const auto byte : zone.settings) std::wcout << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned>(byte) << L' ';
                    std::wcout << std::dec << std::setfill(L' ');
                }
                std::wcout << L'\n';
            }
        }
        std::wcout << L"    path: " << device.path << L'\n';
    }
#else
    std::cout << "HID++ discovery is only implemented on Windows.\n";
#endif
}

void print_hid_devices() {
#ifdef _WIN32
    const auto devices = logipro::enumerate_logitech_hid();
    std::wcout << L"Logitech HID interfaces: " << devices.size() << L'\n';
    for (const auto& device : devices) {
        std::wcout << L"  " << device.product_id << L"  "
                   << (device.product.empty() ? L"(unnamed)" : device.product) << L'\n'
                   << L"    usage: 0x" << std::hex << std::setw(4) << std::setfill(L'0') << device.usage_page
                   << L":0x" << std::setw(4) << device.usage << std::dec << std::setfill(L' ') << L'\n'
                   << L"    reports: input=" << device.input_report_length
                   << L" output=" << device.output_report_length
                   << L" feature=" << device.feature_report_length << L'\n'
                   << L"    path: " << device.path << L'\n';
    }
#else
    std::cout << "HID enumeration is only implemented on Windows.\n";
#endif
}

}

namespace logipro {

int run(int argc, char* argv[]) {
    std::vector<MouseBinding> bindings;
    struct ProfileBinding { std::uint8_t button; std::array<std::uint8_t, 4> spec; };
    std::vector<ProfileBinding> profile_bindings;
    bool watch_buttons = false;
    bool capture_hid = false;
    bool profile_restore = false;
    bool profile_lighting_off = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
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
            MouseButton button = MouseButton::Forward;
            if (argument == "--bind-left") button = MouseButton::Left;
            else if (argument == "--bind-right") button = MouseButton::Right;
            else if (argument == "--bind-middle") button = MouseButton::Middle;
            else if (argument == "--bind-back") button = MouseButton::Back;
            bindings.push_back({button, *key});
            continue;
        }
        std::cerr << "Unknown argument: " << argument << '\n';
        return 2;
    }

    if (capture_hid) {
        return logipro::capture_raw_input();
    }
    if (!bindings.empty()) {
        return logipro::run_mouse_bindings(bindings);
    }
    if (!profile_bindings.empty()) {
        for (const auto& binding : profile_bindings) {
            const int result = logipro::bind_onboard_button(binding.button, binding.spec);
            if (result != 0) return result;
        }
        return 0;
    }
    if (profile_restore) {
        return logipro::restore_onboard_profile();
    }
    if (profile_lighting_off) {
        return logipro::disable_onboard_lighting();
    }
    if (watch_buttons) {
        return logipro::watch_mouse_buttons();
    }

    print_help();
    return 0;
}

}
