#pragma once

#include <string>
#include <vector>

struct wl_display;

namespace gsr {
    struct WaylandMonitor {
        std::string name;
        // wl_output mode size — pixel-density resolution, what to use for layout
        // that ends up in the framebuffer (GL viewport, mgl::Window set_size,
        // sprite/text sizes — those all live in buffer pixels).
        int width = 0;
        int height = 0;
        // xdg_output logical size — what the compositor uses for layer-shell
        // anchors, margins, and wp_viewport.set_destination. On a 4K monitor
        // at 2.0 scale this is 1920x1080; on 1.5 scale it's the fractional
        // logical size the compositor advertised.
        int logical_width = 0;
        int logical_height = 0;
    };

    struct wl_display *wayland_connect_to_host();
    std::vector<WaylandMonitor> wayland_get_monitors(struct wl_display *display);
}
