#include "logipro/hid.hpp"
#include "logipro/hidpp.hpp"

#include <gtk/gtk.h>

#include <climits>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

struct UiState {
    GtkTextView* output = nullptr;
    GtkLabel* status = nullptr;
};

void append_utf8(std::string& output, std::uint32_t codepoint) {
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

std::string narrow(const std::wstring& value) {
    std::string output;
    for (std::size_t index = 0; index < value.size(); ++index) {
        std::uint32_t codepoint = static_cast<std::uint32_t>(value[index]);
#if WCHAR_MAX == 0xffff
        if (codepoint >= 0xd800 && codepoint <= 0xdbff && index + 1 < value.size()) {
            const auto low = static_cast<std::uint32_t>(value[index + 1]);
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

std::string probe_text() {
    const auto devices = logipro::probe_logitech_hidpp(logipro::enumerate_logitech_hid());
    std::ostringstream output;
    output << "HID++ devices: " << devices.size() << "\n\n";
    for (const auto& device : devices) {
        output << (device.product.empty() ? "(unnamed)" : narrow(device.product))
               << "  index=0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(device.device_index)
               << "  protocol=" << std::dec << static_cast<unsigned>(device.protocol_major) << '.' << static_cast<unsigned>(device.protocol_minor) << "\n";
        output << "Features: " << device.features.size() << "\n";
        if (device.onboard_profiles.readable) {
            output << "Onboard profiles: format " << static_cast<unsigned>(device.onboard_profiles.profile_format)
                   << ", buttons " << static_cast<unsigned>(device.onboard_profiles.button_count)
                   << ", active sector " << device.onboard_profiles.active_sector
                   << (device.onboard_profiles.active_profile_crc_valid ? " (CRC OK)" : " (CRC BAD)") << "\n";
            for (std::size_t index = 0; index < device.onboard_profiles.active_lighting.size(); ++index) {
                output << "  Lighting " << index + 1 << ": profile effect 0x" << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned>(device.onboard_profiles.active_lighting[index][0]) << std::dec << "\n";
            }
        }
        if (device.lighting.readable) {
            output << "Live lighting: " << static_cast<unsigned>(device.lighting.zone_count) << " zones, "
                   << (device.lighting.software_control == 0 ? "firmware control" : "software control") << "\n";
        }
        output << "\nPath: " << narrow(device.path) << "\n";
    }
    if (devices.empty()) output << "No compatible Logitech HID++ device was found.\n";
    return output.str();
}

void refresh_output(UiState* state) {
    const auto text = probe_text();
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(state->output), text.c_str(), -1);
    gtk_label_set_text(state->status, "Probe complete");
}

void refresh_clicked(GtkButton*, gpointer data) {
    refresh_output(static_cast<UiState*>(data));
}

void lighting_off_clicked(GtkButton*, gpointer data) {
    auto* state = static_cast<UiState*>(data);
    const int result = logipro::disable_onboard_lighting();
    if (result == 0) {
        refresh_output(state);
        gtk_label_set_text(state->status, "Onboard lighting disabled");
    } else {
        gtk_label_set_text(state->status, "Lighting write failed");
    }
}

void activate(GtkApplication* application, gpointer) {
    auto* window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_title(window, "LogiPro");
    gtk_window_set_default_size(window, 820, 600);
    auto* state = new UiState;
    g_object_set_data_full(G_OBJECT(window), "logipro-state", state, [](gpointer data) { delete static_cast<UiState*>(data); });
    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(root, 10);
    gtk_widget_set_margin_end(root, 10);
    gtk_widget_set_margin_top(root, 10);
    gtk_widget_set_margin_bottom(root, 10);
    gtk_window_set_child(window, root);
    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(root), actions);
    auto* refresh = gtk_button_new_with_label("Refresh");
    auto* lighting_off = gtk_button_new_with_label("Disable onboard lighting");
    gtk_box_append(GTK_BOX(actions), refresh);
    gtk_box_append(GTK_BOX(actions), lighting_off);
    state->status = GTK_LABEL(gtk_label_new("Starting probe..."));
    gtk_widget_set_halign(GTK_WIDGET(state->status), GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(state->status));
    auto* scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(root), scroll);
    state->output = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(state->output, FALSE);
    gtk_text_view_set_monospace(state->output, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(state->output));
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_clicked), state);
    g_signal_connect(lighting_off, "clicked", G_CALLBACK(lighting_off_clicked), state);
    gtk_window_present(window);
    refresh_output(state);
}

}

int main(int argc, char** argv) {
    auto* application = gtk_application_new("com.morenoland.logipro", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(activate), nullptr);
    const int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
