#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace logipro {

struct HidDeviceInfo {
    std::wstring path;
    std::wstring product;
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
    std::uint16_t usage_page = 0;
    std::uint16_t usage = 0;
    std::uint16_t input_report_length = 0;
    std::uint16_t output_report_length = 0;
    std::uint16_t feature_report_length = 0;
};

std::vector<HidDeviceInfo> enumerate_logitech_hid();

}
