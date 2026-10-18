#include "../include/WaylandHostBridge.hpp"

#include <memory>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <wayland-client.h>
#include "xdg-output-unstable-v1-client-protocol.h"

#define GSR_UI_UNIX_SOCKET_DOMAIN_FD 3

namespace gsr {
    // Returns the length of the app path, or 0 on failure
    static size_t get_flatpak_app_path(char *path, size_t max_path) {
        if(max_path == 0)
            return 0;

        path[0] = '\0';
        FILE *f = fopen("/.flatpak-info", "rb");
        if(!f)
            return 0;

        char line[4096];

        while(fgets(line, sizeof(line), f)) {
            size_t line_len = strlen(line);
            if(line_len > 0 && line[line_len - 1] == '\n') {
                line[line_len - 1] = '\0';
                line_len -= 1;
            }

            if(line_len >= 9 && memcmp(line, "app-path=", 9) == 0) {
                line_len -= 9;
                snprintf(path, max_path, "%.*s", (int)line_len, line + 9);
                break;
            }
        }

        fclose(f);
        return strlen(path);
    }

    static int recv_fd(int sock) {
        char dummy = 0;
        iovec iov = { &dummy, 1 };
        char cbuf[CMSG_SPACE(sizeof(int))];
        memset(cbuf, 0, sizeof(cbuf));
        msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);

        const ssize_t n = recvmsg(sock, &msg, 0);
        if(n <= 0)
            return -1;

        for(cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if(c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int fd = -1;
                memcpy(&fd, CMSG_DATA(c), sizeof(int));
                return fd;
            }
        }
        return -1;
    }

    static wl_display* connect_via_bridge(const char *bridge_host_path) {
        const char *wayland_display = getenv("WAYLAND_DISPLAY");

        int sv[2];
        if(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
            perror("WaylandHostBridge: socketpair");
            return nullptr;
        }

        const pid_t pid = fork();
        if(pid < 0) {
            perror("WaylandHostBridge: fork");
            close(sv[0]);
            close(sv[1]);
            return nullptr;
        }

        if(pid == 0) {
            close(sv[0]);
            if(sv[1] != GSR_UI_UNIX_SOCKET_DOMAIN_FD) {
                dup2(sv[1], GSR_UI_UNIX_SOCKET_DOMAIN_FD);
                close(sv[1]);
            }
            const int flags = fcntl(GSR_UI_UNIX_SOCKET_DOMAIN_FD, F_GETFD);
            if(flags >= 0)
                fcntl(GSR_UI_UNIX_SOCKET_DOMAIN_FD, F_SETFD, flags & ~FD_CLOEXEC);

            char forward_fd_arg[32];
            snprintf(forward_fd_arg, sizeof(forward_fd_arg), "--forward-fd=%d", GSR_UI_UNIX_SOCKET_DOMAIN_FD);

            execlp("flatpak-spawn", "flatpak-spawn", "--host", forward_fd_arg, bridge_host_path, wayland_display, (char*)nullptr);
            _exit(127);
        }

        close(sv[1]);
        const int wayland_fd = recv_fd(sv[0]);
        close(sv[0]);

        int status = 0;
        waitpid(pid, &status, 0);

        if(wayland_fd < 0) {
            fprintf(stderr, "WaylandHostBridge: gsr-wayland-bridge did not return a wayland fd\n");
            return nullptr;
        }

        wl_display *dpy = wl_display_connect_to_fd(wayland_fd);
        if(!dpy) {
            close(wayland_fd);
            fprintf(stderr, "WaylandHostBridge: wl_display_connect_to_fd failed\n");
            return nullptr;
        }
        return dpy;
    }

    wl_display* wayland_connect_to_host() {
        char gsr_wayland_bridge_path[PATH_MAX];
        const size_t path_len = get_flatpak_app_path(gsr_wayland_bridge_path, sizeof(gsr_wayland_bridge_path));
        const char *gsr_wayland_bridge_suffix = "/bin/gsr-wayland-bridge";
        const size_t suffix_len = strlen(gsr_wayland_bridge_suffix);

        if(path_len > 0 && path_len + suffix_len + 1 <= sizeof(gsr_wayland_bridge_path)) {
            memcpy(gsr_wayland_bridge_path + path_len, gsr_wayland_bridge_suffix, suffix_len);
            gsr_wayland_bridge_path[path_len + suffix_len] = '\0';

            struct wl_display *dpy = connect_via_bridge(gsr_wayland_bridge_path);
            if(dpy)
                return dpy;

            fprintf(stderr, "WaylandHostBridge: falling back to sandboxed wl_display_connect\n");
        }

        return wl_display_connect(nullptr);
    }

    namespace {
        struct OutputEntry {
            wl_output *output = nullptr;
            zxdg_output_v1 *xdg_output = nullptr;
            WaylandMonitor monitor;
            int mode_width = 0;
            int mode_height = 0;
            int logical_width = 0;
            int logical_height = 0;
        };

        struct OutputGlobal {
            uint32_t name = 0;
            uint32_t version = 0;
        };

        struct RegistryCollector {
            std::vector<OutputGlobal> outputs;
            uint32_t xdg_output_manager_name = 0;
            uint32_t xdg_output_manager_version = 0;
        };

        void output_handle_geometry(void *data, wl_output*, int32_t, int32_t, int32_t, int32_t,
                                    int32_t, const char*, const char *model, int32_t) {
            OutputEntry *entry = static_cast<OutputEntry*>(data);
            if(entry->monitor.name.empty() && model)
                entry->monitor.name = model;
        }

        void output_handle_mode(void *data, wl_output*, uint32_t flags, int32_t width, int32_t height, int32_t) {
            OutputEntry *entry = static_cast<OutputEntry*>(data);
            if(flags & WL_OUTPUT_MODE_CURRENT) {
                entry->mode_width = width;
                entry->mode_height = height;
            }
        }

        void output_handle_done(void*, wl_output*) {}
        void output_handle_scale(void*, wl_output*, int32_t) {}

        void output_handle_name(void *data, wl_output*, const char *name) {
            OutputEntry *entry = static_cast<OutputEntry*>(data);
            if(name)
                entry->monitor.name = name;
        }

        void output_handle_description(void*, wl_output*, const char*) {}

        const wl_output_listener output_listener = {
            output_handle_geometry,
            output_handle_mode,
            output_handle_done,
            output_handle_scale,
            output_handle_name,
            output_handle_description,
        };

        void xdg_output_handle_logical_position(void*, zxdg_output_v1*, int32_t, int32_t) {}

        void xdg_output_handle_logical_size(void *data, zxdg_output_v1*, int32_t width, int32_t height) {
            OutputEntry *entry = static_cast<OutputEntry*>(data);
            entry->logical_width = width;
            entry->logical_height = height;
        }

        void xdg_output_handle_done(void*, zxdg_output_v1*) {}
        void xdg_output_handle_name(void*, zxdg_output_v1*, const char*) {}
        void xdg_output_handle_description(void*, zxdg_output_v1*, const char*) {}

        const zxdg_output_v1_listener xdg_output_listener = {
            xdg_output_handle_logical_position,
            xdg_output_handle_logical_size,
            xdg_output_handle_done,
            xdg_output_handle_name,
            xdg_output_handle_description,
        };

        void registry_global(void *data, wl_registry*, uint32_t name, const char *interface, uint32_t version) {
            RegistryCollector *collector = static_cast<RegistryCollector*>(data);
            if(strcmp(interface, wl_output_interface.name) == 0) {
                collector->outputs.push_back({name, version});
            } else if(strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
                collector->xdg_output_manager_name = name;
                collector->xdg_output_manager_version = version;
            }
        }

        void registry_global_remove(void*, wl_registry*, uint32_t) {}

        const wl_registry_listener registry_listener = {
            registry_global,
            registry_global_remove,
        };
    }

    std::vector<WaylandMonitor> wayland_get_monitors(wl_display *display) {
        if(!display)
            return {};

        RegistryCollector collector;
        wl_registry *registry = wl_display_get_registry(display);
        if(!registry)
            return {};
        wl_registry_add_listener(registry, &registry_listener, &collector);
        wl_display_roundtrip(display);

        zxdg_output_manager_v1 *xdg_output_manager = nullptr;
        if(collector.xdg_output_manager_name) {
            const uint32_t bind_version = collector.xdg_output_manager_version < 3 ? collector.xdg_output_manager_version : 3;
            xdg_output_manager = static_cast<zxdg_output_manager_v1*>(
                wl_registry_bind(registry, collector.xdg_output_manager_name, &zxdg_output_manager_v1_interface, bind_version));
        }

        std::vector<std::unique_ptr<OutputEntry>> entries;
        entries.reserve(collector.outputs.size());
        for(const OutputGlobal &g : collector.outputs) {
            const uint32_t bind_version = g.version < 4 ? g.version : 4;
            auto entry = std::make_unique<OutputEntry>();
            entry->output = static_cast<wl_output*>(wl_registry_bind(registry, g.name, &wl_output_interface, bind_version));
            if(!entry->output)
                continue;
            wl_output_add_listener(entry->output, &output_listener, entry.get());
            if(xdg_output_manager) {
                entry->xdg_output = zxdg_output_manager_v1_get_xdg_output(xdg_output_manager, entry->output);
                if(entry->xdg_output)
                    zxdg_output_v1_add_listener(entry->xdg_output, &xdg_output_listener, entry.get());
            }
            entries.push_back(std::move(entry));
        }
        wl_display_roundtrip(display);

        std::vector<WaylandMonitor> result;
        result.reserve(entries.size());
        for(auto &entry : entries) {
            entry->monitor.width = entry->mode_width;
            entry->monitor.height = entry->mode_height;
            // Fall back to mode dimensions when xdg-output isn't advertised; on
            // unscaled outputs logical == mode anyway.
            entry->monitor.logical_width = entry->logical_width > 0 ? entry->logical_width : entry->mode_width;
            entry->monitor.logical_height = entry->logical_height > 0 ? entry->logical_height : entry->mode_height;
            result.push_back(std::move(entry->monitor));
            if(entry->xdg_output)
                zxdg_output_v1_destroy(entry->xdg_output);
            wl_output_destroy(entry->output);
        }
        if(xdg_output_manager)
            zxdg_output_manager_v1_destroy(xdg_output_manager);
        wl_registry_destroy(registry);
        return result;
    }
}
