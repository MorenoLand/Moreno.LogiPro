#pragma once

#include <cstdint>
#include <vector>

namespace logipro {

enum class MouseButton : std::uint8_t { Left, Right, Middle, Back, Forward };

struct MouseBinding {
    MouseButton button;
    std::uint16_t virtual_key;
};

int run_mouse_bindings(const std::vector<MouseBinding>& bindings);
int watch_mouse_buttons();

}
