#pragma once

#include "logipro/export.hpp"

#include <cstdint>
#include <vector>

namespace logipro {

enum class MouseButton : std::uint8_t { Left, Right, Middle, Back, Forward };

struct MouseBinding {
    MouseButton button;
    std::uint16_t virtual_key;
};

LOGIPRO_API int run_mouse_bindings(const std::vector<MouseBinding>& bindings);
LOGIPRO_API int watch_mouse_buttons();

}
