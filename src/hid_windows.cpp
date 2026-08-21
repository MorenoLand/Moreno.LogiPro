#include "logipro/hid.hpp"

#ifdef _WIN32

#include <windows.h>
#include <initguid.h>
#include <hidclass.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>

#include <array>
#include <vector>

namespace {

class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }

private:
    HANDLE value_;
};

HANDLE open_path(const wchar_t* path) {
    for (const DWORD access : {static_cast<DWORD>(GENERIC_READ | GENERIC_WRITE), static_cast<DWORD>(GENERIC_READ), static_cast<DWORD>(0)}) {
        HANDLE handle = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            return handle;
        }
    }
    return INVALID_HANDLE_VALUE;
}

std::wstring product_string(HANDLE handle) {
    std::array<wchar_t, 256> buffer{};
    if (HidD_GetProductString(handle, buffer.data(), static_cast<ULONG>(sizeof(buffer)))) {
        return buffer.data();
    }
    return {};
}

}

namespace logipro {

std::vector<HidDeviceInfo> enumerate_logitech_hid() {
    std::vector<HidDeviceInfo> devices;
    HDEVINFO device_set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_HID, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (device_set == INVALID_HANDLE_VALUE) {
        return devices;
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(device_set, nullptr, &GUID_DEVINTERFACE_HID, index, &interface_data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(device_set, &interface_data, nullptr, 0, &required, nullptr);
        if (required == 0) {
            continue;
        }

        std::vector<std::uint8_t> detail_buffer(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(device_set, &interface_data, detail, required, nullptr, nullptr)) {
            continue;
        }

        Handle handle(open_path(detail->DevicePath));
        if (handle.get() == INVALID_HANDLE_VALUE) {
            continue;
        }

        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (!HidD_GetAttributes(handle.get(), &attributes) || attributes.VendorID != 0x046d) {
            continue;
        }

        HidDeviceInfo device;
        device.path = detail->DevicePath;
        device.product = product_string(handle.get());
        device.vendor_id = attributes.VendorID;
        device.product_id = attributes.ProductID;

        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (HidD_GetPreparsedData(handle.get(), &preparsed)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
                device.usage_page = caps.UsagePage;
                device.usage = caps.Usage;
                device.input_report_length = caps.InputReportByteLength;
                device.output_report_length = caps.OutputReportByteLength;
                device.feature_report_length = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(preparsed);
        }

        devices.push_back(std::move(device));
    }

    SetupDiDestroyDeviceInfoList(device_set);
    return devices;
}

}

#else

namespace logipro {

std::vector<HidDeviceInfo> enumerate_logitech_hid() {
    return {};
}

}

#endif
