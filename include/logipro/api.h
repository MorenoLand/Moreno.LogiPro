#pragma once

#include "logipro/export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct logipro_snapshot logipro_snapshot_t;

typedef struct {
    const char* path;
    const char* product;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t usage_page;
    uint16_t usage;
    uint16_t input_report_length;
    uint16_t output_report_length;
    uint16_t feature_report_length;
} logipro_hid_info_t;

typedef struct {
    uint16_t id;
    uint8_t index;
    uint8_t present;
    uint8_t version;
    uint8_t type;
} logipro_feature_info_t;

typedef struct {
    uint8_t index;
    uint16_t current_dpi;
    uint16_t min_dpi;
    uint16_t max_dpi;
    uint16_t step;
    uint16_t default_dpi;
    size_t value_count;
} logipro_dpi_sensor_info_t;

typedef struct {
    const char* path;
    const char* product;
    uint16_t product_id;
    uint8_t device_index;
    uint8_t protocol_major;
    uint8_t protocol_minor;
    size_t feature_count;
    uint8_t onboard_profiles_readable;
    uint8_t onboard_feature_index;
    uint8_t onboard_mode_readable;
    uint8_t onboard_mode;
    uint8_t profile_format;
    uint8_t button_count;
    uint16_t sector_size;
    uint8_t active_profile_readable;
    uint16_t active_sector;
    uint8_t active_profile_crc_valid;
    uint8_t button_offset;
    uint8_t active_lighting_readable;
    size_t active_lighting_count;
    uint8_t dpi_profile_readable;
    uint8_t dpi_profile_count;
    uint8_t dpi_default_index;
    uint8_t dpi_shift_index;
    uint16_t dpi_profile_values[5];
    uint8_t dpi_readable;
    uint8_t dpi_feature_index;
    size_t dpi_sensor_count;
    uint8_t lighting_readable;
    uint8_t lighting_feature_index;
    uint8_t lighting_declared_zone_count;
    uint16_t lighting_nv_capabilities;
    uint16_t lighting_extended_capabilities;
    uint8_t lighting_software_control_readable;
    uint8_t lighting_software_control;
    uint8_t lighting_sync_events;
    size_t lighting_zone_records;
    uint8_t battery_readable;
    uint16_t battery_feature_id;
    uint8_t battery_feature_index;
    uint8_t battery_percentage_readable;
    uint8_t battery_percentage;
    uint8_t battery_percentage_estimated;
    uint8_t battery_voltage_readable;
    uint16_t battery_voltage_mv;
    uint8_t battery_status;
    uint8_t battery_level;
    uint8_t battery_flags;
} logipro_device_info_t;

typedef struct {
    uint8_t requested_zone;
    uint8_t info_readable;
    uint8_t info_zone;
    uint16_t location;
    uint8_t effect_count;
    uint8_t effect_readable;
    uint8_t effect_zone;
    uint8_t effect;
    uint8_t settings_readable;
    uint8_t settings_zone;
} logipro_lighting_zone_info_t;

typedef struct {
    uint8_t button;
    uint16_t virtual_key;
} logipro_mouse_binding_t;

enum {
    LOGIPRO_OK = 0,
    LOGIPRO_INVALID_ARGUMENT = 1,
    LOGIPRO_NOT_FOUND = 2,
    LOGIPRO_IO_ERROR = 3,
    LOGIPRO_UNSUPPORTED = 4,
    LOGIPRO_INTERNAL_ERROR = 5
};

enum {
    LOGIPRO_MOUSE_LEFT = 0,
    LOGIPRO_MOUSE_RIGHT = 1,
    LOGIPRO_MOUSE_MIDDLE = 2,
    LOGIPRO_MOUSE_BACK = 3,
    LOGIPRO_MOUSE_FORWARD = 4
};

LOGIPRO_C_API int logipro_snapshot_create(logipro_snapshot_t** out_snapshot);
LOGIPRO_C_API void logipro_snapshot_destroy(logipro_snapshot_t* snapshot);
LOGIPRO_C_API size_t logipro_snapshot_hid_count(const logipro_snapshot_t* snapshot);
LOGIPRO_C_API int logipro_snapshot_get_hid(const logipro_snapshot_t* snapshot, size_t index, logipro_hid_info_t* out_info);
LOGIPRO_C_API size_t logipro_snapshot_device_count(const logipro_snapshot_t* snapshot);
LOGIPRO_C_API int logipro_snapshot_get_device(const logipro_snapshot_t* snapshot, size_t index, logipro_device_info_t* out_info);
LOGIPRO_C_API int logipro_snapshot_get_feature(const logipro_snapshot_t* snapshot, size_t device_index, size_t feature_index, logipro_feature_info_t* out_info);
LOGIPRO_C_API int logipro_snapshot_get_button(const logipro_snapshot_t* snapshot, size_t device_index, uint8_t button, uint8_t out_spec[4]);
LOGIPRO_C_API int logipro_snapshot_get_dpi_sensor(const logipro_snapshot_t* snapshot, size_t device_index, size_t sensor_index, logipro_dpi_sensor_info_t* out_info);
LOGIPRO_C_API int logipro_snapshot_get_dpi_value(const logipro_snapshot_t* snapshot, size_t device_index, size_t sensor_index, size_t value_index, uint16_t* out_dpi);
LOGIPRO_C_API int logipro_snapshot_get_active_lighting(const logipro_snapshot_t* snapshot, size_t device_index, size_t lighting_index, uint8_t out_record[11]);
LOGIPRO_C_API int logipro_snapshot_get_lighting_zone(const logipro_snapshot_t* snapshot, size_t device_index, size_t zone_index, logipro_lighting_zone_info_t* out_info);
LOGIPRO_C_API int logipro_profile_bind(uint8_t button, const uint8_t spec[4]);
LOGIPRO_C_API int logipro_dpi_set(uint8_t sensor, uint16_t dpi);
LOGIPRO_C_API int logipro_profile_dpi_set(uint8_t slot, uint16_t dpi);
LOGIPRO_C_API int logipro_profile_dpi_set_default(uint8_t slot);
LOGIPRO_C_API int logipro_profile_restore(void);
LOGIPRO_C_API int logipro_profile_lighting_off(void);
LOGIPRO_C_API int logipro_capture_hid(void);
LOGIPRO_C_API int logipro_capture_raw_input(void);
LOGIPRO_C_API int logipro_watch_buttons(void);
LOGIPRO_C_API int logipro_run_mouse_bindings(const logipro_mouse_binding_t* bindings, size_t count);
LOGIPRO_C_API void logipro_debug_set_enabled(int enabled);
LOGIPRO_C_API int logipro_debug_is_enabled(void);
LOGIPRO_C_API const char* logipro_last_error(void);

#ifdef __cplusplus
}
#endif
