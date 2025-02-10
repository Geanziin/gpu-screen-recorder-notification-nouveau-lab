#include <mglpp/mglpp.hpp>
#include <mglpp/window/Window.hpp>
#include <mglpp/window/Event.hpp>
#include <mglpp/system/Clock.hpp>
#include <mglpp/graphics/Rectangle.hpp>
#include <mglpp/system/MemoryMappedFile.hpp>
#include <mglpp/graphics/Font.hpp>
#include <mglpp/graphics/Text.hpp>
#include <mglpp/graphics/Texture.hpp>
#include <mglpp/graphics/Sprite.hpp>

#include <stdio.h>
#include <string.h>
#include <array>
#include <map>
#include <assert.h>
#include <unistd.h>
#include <limits.h>
#include <poll.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>

extern "C" {
#include <mgl/mgl.h>
}

enum class State {
    SLIDE_IN_WINDOW,
    SLIDE_IN_CONTENT,
    FADE_IN_CONTENT,
    FADE_OUT_CONTENT,
    SLIDE_OUT_CONTENT,
    SLIDE_OUT_WINDOW,
    PAUSE
};

struct StateWithPayload {
    State state;
    double time_in_state_sec;
};

// Linear interpolation
static double interpolate(double a, double b, double interpolation) {
    return a + (b - a) * interpolation;
}

static double min_double(double a, double b) {
    return a < b ? a : b;
}

#define _NET_WM_STATE_REMOVE  0
#define _NET_WM_STATE_ADD     1
#define _NET_WM_STATE_TOGGLE  2

static Bool set_window_wm_state(Display *display, Window window, Atom atom) {
    Atom net_wm_state_atom = XInternAtom(display, "_NET_WM_STATE", False);
    if(!net_wm_state_atom) {
        fprintf(stderr, "Error: failed to find atom _NET_WM_STATE\n");
        return False;
    }

    XClientMessageEvent xclient;
    memset(&xclient, 0, sizeof(xclient));

    xclient.type = ClientMessage;
    xclient.window = window;
    xclient.message_type = net_wm_state_atom;
    xclient.format = 32;
    xclient.data.l[0] = _NET_WM_STATE_ADD;
    xclient.data.l[1] = atom;
    xclient.data.l[2] = 0;
    xclient.data.l[3] = 0;
    xclient.data.l[4] = 0;

    XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, (XEvent*)&xclient);
    XFlush(display);
    return True;
}

static Bool make_window_sticky(Display* display, Window window) {
    Atom net_wm_state_sticky_atom = XInternAtom(display, "_NET_WM_STATE_STICKY", False);
    if(!net_wm_state_sticky_atom) {
        fprintf(stderr, "Error: failed to find atom _NET_WM_STATE_STICKY\n");
        return False;
    }
    
    return set_window_wm_state(display, window, net_wm_state_sticky_atom);
}

static void make_window_click_through(Display *display, Window window) {
    XRectangle rect;
    memset(&rect, 0, sizeof(rect));
    XShapeCombineRectangles(display, window, ShapeInput, 0, 0, &rect, 1, ShapeSet, YXBanded);
}

static void set_window_clip_region(Display *display, Window window, mgl::vec2i pos, mgl::vec2i size) {
    if(size.x < 0)
        size.x = 0;
    if(size.y < 0)
        size.y = 0;
    XRectangle rectangle = {(short)pos.x, (short)pos.y, (unsigned short)size.x, (unsigned short)size.y};
    XShapeCombineRectangles(display, window, ShapeBounding, 0, 0, &rectangle, 1, ShapeSet, YXBanded);
}

static bool is_xwayland(Display *display) {
    int opcode, event, error;
    return XQueryExtension(display, "XWAYLAND", &opcode, &event, &error);
}

static void usage() {
    fprintf(stderr, "usage: gsr-notify --text text --timeout timeout [--icon filepath] [--icon-color color] [--bg-color color]\n");
    fprintf(stderr, "options:\n");
    fprintf(stderr, "  --text        The text to display in the notification. Required.\n");
    fprintf(stderr, "  --timeout     The time to display the notification in seconds (excluding animation time), expected to be a floating point number. Required.\n");
    fprintf(stderr, "  --icon        A path to an image file to display on the left side of the text. This can also be either \"record\", \"replay\" or \"stream\" to use built-in images. Optional.\n");
    fprintf(stderr, "  --icon-color  The color to display the icon as in hex format, for example FF0000. Optional, set to FFFFFF by default.\n");
    fprintf(stderr, "  --bg-color    The notification background (and side bar) color in hex format, for example FF0000. Optional, set to 76b900 by default.\n");
    fprintf(stderr, "examples:\n");
    fprintf(stderr, "  gsr-notify --text 'Recording has started' --timeout 3.0\n");
    fprintf(stderr, "  gsr-notify --text 'Recording has started' --timeout 3.0 --icon record\n");
    fprintf(stderr, "  gsr-notify --text 'Recording has started' --timeout 3.0 --icon '/usr/share/gpu-screen-recorder/images/record.png'\n");
    fprintf(stderr, "  gsr-notify --text 'Recording has started' --timeout 3.0 --icon '/usr/share/gpu-screen-recorder/images/record.png' --icon-color FF0000\n");
    exit(1);
}

static int hex_character_to_number(char c) {
    if(c >= '0' && c <= '9')
        return c - '0';
    else if(c >= 'a' && c <= 'f')
        return 10 + c - 'a';
    else if(c >= 'A' && c <= 'F')
        return 10 + c - 'A';
    else
        return -1;
}

static mgl::Color parse_hex_color(const char *str) {
    const int len = strlen(str);
    if(len != 6) {
        fprintf(stderr, "error: expected icon-color to be 6 characters long, was: %d\n", len);
        usage();
    }

    mgl::Color color;
    uint8_t *comp = &color.r;
    for(int i = 0; i < len; i += 2) {
        const int val1 = hex_character_to_number(str[i + 0]);
        const int val2 = hex_character_to_number(str[i + 1]);
        if(val1 == -1 || val2 == -1) {
            fprintf(stderr, "error: icon-color is an invalid hex color: '%s'\n", str);
            usage();
        }
        comp[i / 2] = (val1 << 4) | val2;
    }
    return color;
}

// Returns the first monitor if not found. Assumes there is at least one monitor connected.
static const mgl_monitor* find_monitor_at_position(mgl::Window &window, mgl::vec2i pos) {
    const mgl_window *win = window.internal_window();
    assert(win->num_monitors > 0);
    for(int i = 0; i < win->num_monitors; ++i) {
        const mgl_monitor *mon = &win->monitors[i];
        if(mgl::IntRect({ mon->pos.x, mon->pos.y }, { mon->size.x, mon->size.y }).contains(pos))
            return mon;
    }
    return &win->monitors[0];
}

static bool window_has_atom(Display *dpy, Window window, Atom atom) {
    Atom type;
    unsigned long len, bytes_left;
    int format;
    unsigned char *properties = NULL;
    if(XGetWindowProperty(dpy, window, atom, 0, 1024, False, AnyPropertyType, &type, &format, &len, &bytes_left, &properties) < Success)
        return false;

    if(properties)
        XFree(properties);

    return type != None;
}

static bool window_is_user_program(Display *dpy, Window window) {
    const Atom net_wm_state_atom = XInternAtom(dpy, "_NET_WM_STATE", False);
    const Atom wm_state_atom = XInternAtom(dpy, "WM_STATE", False);
    return window_has_atom(dpy, window, net_wm_state_atom) || window_has_atom(dpy, window, wm_state_atom);
}

static Window window_get_target_window_child(Display *display, Window window) {
    if(window == None)
        return None;

    if(window_is_user_program(display, window))
        return window;

    Window root;
    Window parent;
    Window *children = nullptr;
    unsigned int num_children = 0;
    if(!XQueryTree(display, window, &root, &parent, &children, &num_children) || !children)
        return None;

    Window found_window = None;
    for(int i = num_children - 1; i >= 0; --i) {
        if(children[i] && window_is_user_program(display, children[i])) {
            found_window = children[i];
            goto finished;
        }
    }

    for(int i = num_children - 1; i >= 0; --i) {
        if(children[i]) {
            Window win = window_get_target_window_child(display, children[i]);
            if(win) {
                found_window = win;
                goto finished;
            }
        }
    }

    finished:
    XFree(children);
    return found_window;
}

static bool window_has_title(Display *dpy, Window window) {
    bool result = false;
    const Atom net_wm_name_atom = XInternAtom(dpy, "_NET_WM_NAME", False);
    const Atom wm_name_atom = XInternAtom(dpy, "WM_NAME", False);
    const Atom utf8_string_atom = XInternAtom(dpy, "UTF8_STRING", False);

    Atom type = None;
    int format = 0;
    unsigned long num_items = 0;
    unsigned long bytes_left = 0;
    unsigned char *data = NULL;
    XGetWindowProperty(dpy, window, net_wm_name_atom, 0, 1024, False, utf8_string_atom, &type, &format, &num_items, &bytes_left, &data);

    if(type == utf8_string_atom && format == 8 && data) {
        result = true;
        goto done;
    }

    if(data)
        XFree(data);

    type = None;
    format = 0;
    num_items = 0;
    bytes_left = 0;
    data = NULL;
    XGetWindowProperty(dpy, window, wm_name_atom, 0, 1024, False, 0, &type, &format, &num_items, &bytes_left, &data);

    if((type == XA_STRING || type == utf8_string_atom) && data) {
        result = true;
        goto done;
    }

    done:
    if(data)
        XFree(data);
    return result;
}

static mgl::vec2i get_cursor_position(Display *dpy, Window *window) {
    Window root_window = None;
    *window = None;
    int dummy_i;
    unsigned int dummy_u;
    mgl::vec2i root_pos;
    XQueryPointer(dpy, DefaultRootWindow(dpy), &root_window, window, &root_pos.x, &root_pos.y, &dummy_i, &dummy_i, &dummy_u);

    const Window direct_window = *window;
    *window = window_get_target_window_child(dpy, *window);
    // HACK: Count some other x11 windows as having an x11 window focused. Some games seem to create an Input window and that gets focused.
    if(!*window) {
        XWindowAttributes attr;
        memset(&attr, 0, sizeof(attr));
        XGetWindowAttributes(dpy, direct_window, &attr);
        if(attr.c_class == InputOnly && !window_has_title(dpy, direct_window))
            *window = direct_window;
    }
    return root_pos;
}

typedef struct {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long input_mode;
    unsigned long status;
} MotifHints;

#define MWM_HINTS_DECORATIONS   2

#define MWM_DECOR_NONE          0
#define MWM_DECOR_ALL           1

static void window_set_decorations_visible(Display *display, Window window, bool visible) {
    const Atom motif_wm_hints_atom = XInternAtom(display, "_MOTIF_WM_HINTS", False);
    MotifHints motif_hints;
    memset(&motif_hints, 0, sizeof(motif_hints));
    motif_hints.flags = MWM_HINTS_DECORATIONS;
    motif_hints.decorations = visible ? MWM_DECOR_ALL : MWM_DECOR_NONE;
    XChangeProperty(display, window, motif_wm_hints_atom, motif_wm_hints_atom, 32, PropModeReplace, (unsigned char*)&motif_hints, sizeof(motif_hints) / sizeof(long));
}

static bool create_window_get_center_position_kde(Display *display, mgl::vec2i &position) {
    const int size = 1;
    XSetWindowAttributes window_attr;
    window_attr.event_mask = StructureNotifyMask;
    window_attr.background_pixel = 0;
    const Window window = XCreateWindow(display, DefaultRootWindow(display), 0, 0, size, size, 0, CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWEventMask, &window_attr);
    if(!window)
        return false;

    const Atom net_wm_window_type_atom = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    const Atom net_wm_window_type_notification_atom = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    const Atom net_wm_window_type_utility = XInternAtom(display, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    const Atom net_wm_window_opacity = XInternAtom(display, "_NET_WM_WINDOW_OPACITY", False);

    const Atom window_type_atoms[2] = {
        net_wm_window_type_notification_atom,
        net_wm_window_type_utility
    };
    XChangeProperty(display, window, net_wm_window_type_atom, XA_ATOM, 32, PropModeReplace, (const unsigned char*)window_type_atoms, 2L);

    const double alpha = 0.0;
    const unsigned long opacity = (unsigned long)(0xFFFFFFFFul * alpha);
    XChangeProperty(display, window, net_wm_window_opacity, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&opacity, 1L);

    window_set_decorations_visible(display, window, false);

    XSizeHints *size_hints = XAllocSizeHints();
    size_hints->width = size;
    size_hints->height = size;
    size_hints->min_width = size;
    size_hints->min_height = size;
    size_hints->max_width = size;
    size_hints->max_height = size;
    size_hints->flags = PSize | PMinSize | PMaxSize;
    XSetWMNormalHints(display, window, size_hints);
    XFree(size_hints);

    XMapWindow(display, window);
    XFlush(display);

    bool got_data = false;
    const int x_fd = XConnectionNumber(display);
    XEvent xev;
    while(true) {
        struct pollfd poll_fd;
        poll_fd.fd = x_fd;
        poll_fd.events = POLLIN;
        poll_fd.revents = 0;
        const int fds_ready = poll(&poll_fd, 1, 200);
        if(fds_ready == 0) {
            fprintf(stderr, "Error: timed out waiting for ConfigureNotify after XCreateWindow\n");
            break;
        } else if(fds_ready == -1 || !(poll_fd.revents & POLLIN)) {
            continue;
        }

        while(XPending(display)) {
            XNextEvent(display, &xev);
            if(xev.type == ConfigureNotify && xev.xconfigure.window == window) {
                got_data = xev.xconfigure.x > 0 && xev.xconfigure.y > 0;
                position.x = xev.xconfigure.x + xev.xconfigure.width / 2;
                position.y = xev.xconfigure.y + xev.xconfigure.height / 2;
                goto done;
            }
        }
    }

    done:
    XDestroyWindow(display, window);
    XFlush(display);

    return got_data;
}

static bool create_window_get_center_position_gnome(Display *display, mgl::vec2i &position) {
    const int size = 32;
    XSetWindowAttributes window_attr;
    window_attr.event_mask = StructureNotifyMask | ExposureMask;
    window_attr.background_pixel = 0;
    const Window window = XCreateWindow(display, DefaultRootWindow(display), 0, 0, size, size, 0, CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWEventMask, &window_attr);
    if(!window)
        return false;

    const Atom net_wm_window_opacity = XInternAtom(display, "_NET_WM_WINDOW_OPACITY", False);
    const double alpha = 0.0;
    const unsigned long opacity = (unsigned long)(0xFFFFFFFFul * alpha);
    XChangeProperty(display, window, net_wm_window_opacity, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&opacity, 1L);

    window_set_decorations_visible(display, window, false);

    XSizeHints *size_hints = XAllocSizeHints();
    size_hints->width = size;
    size_hints->height = size;
    size_hints->min_width = size;
    size_hints->min_height = size;
    size_hints->max_width = size;
    size_hints->max_height = size;
    size_hints->flags = PSize | PMinSize | PMaxSize;
    XSetWMNormalHints(display, window, size_hints);
    XFree(size_hints);

    XMapWindow(display, window);
    XFlush(display);

    bool got_data = false;
    const int x_fd = XConnectionNumber(display);
    XEvent xev;
    while(true) {
        struct pollfd poll_fd;
        poll_fd.fd = x_fd;
        poll_fd.events = POLLIN;
        poll_fd.revents = 0;
        const int fds_ready = poll(&poll_fd, 1, 200);
        if(fds_ready == 0) {
            fprintf(stderr, "Error: timed out waiting for MapNotify/ConfigureNotify after XCreateWindow\n");
            break;
        } else if(fds_ready == -1 || !(poll_fd.revents & POLLIN)) {
            continue;
        }

        while(XPending(display)) {
            XNextEvent(display, &xev);
            if(xev.type == MapNotify && xev.xmap.window == window) {
                int x = 0;
                int y = 0;
                Window w = None;
                XTranslateCoordinates(display, window, DefaultRootWindow(display), 0, 0, &x, &y, &w);

                got_data = x > 0 && y > 0;
                position.x = x + size / 2;
                position.y = y + size / 2;
                if(got_data)
                    goto done;
            } else if(xev.type == ConfigureNotify && xev.xconfigure.window == window) {
                got_data = xev.xconfigure.x > 0 && xev.xconfigure.y > 0;
                position.x = xev.xconfigure.x + xev.xconfigure.width / 2;
                position.y = xev.xconfigure.y + xev.xconfigure.height / 2;
                if(got_data)
                    goto done;
            }
        }
    }

    done:
    XDestroyWindow(display, window);
    XFlush(display);

    return got_data;
}

static mgl::vec2i create_window_get_center_position(Display *display) {
    mgl::vec2i pos;
    if(!create_window_get_center_position_kde(display, pos)) {
        pos.x = 0;
        pos.y = 0;
        create_window_get_center_position_gnome(display, pos);
    }
    return pos;
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "C"); // Sigh... stupid C

    std::string resources_path;
    if(access("sibs-build/linux_x86_64/debug/gsr-notify", F_OK) == 0) {
        resources_path = "./";
    } else {
#ifdef GSR_NOTIFY_RESOURCES_PATH
        resources_path = GSR_NOTIFY_RESOURCES_PATH "/";
#else
        resources_path = "/usr/share/gsr-notify/";
#endif
    }

    if(argc == 1)
        usage();

    if(argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))
        usage();

    std::map<std::string, const char*> args = {
        {"--text", nullptr},
        {"--timeout", nullptr},
        {"--icon", nullptr},
        {"--icon-color", nullptr},
        {"--bg-color", nullptr},
    };

    for(int i = 1; i < argc; i += 2) {
        auto it = args.find(argv[i]);
        if(it == args.end()) {
            fprintf(stderr, "error: invalid option '%s'\n", argv[i]);
            usage();
        }

        if(i + 1 >= argc) {
            fprintf(stderr, "error: missing value after option '%s'\n", argv[i]);
            usage();
        }

        it->second = argv[i + 1];
    }

    const char *notification_text = args["--text"];
    const char *timeout_str = args["--timeout"];
    const char *icon_filepath = args["--icon"];
    const char *icon_color_str = args["--icon-color"];
    const char *bg_color_str = args["--bg-color"];

    if(!notification_text) {
        fprintf(stderr, "error: missing required option '--text'\n");
        usage();
    }

    if(!timeout_str) {
        fprintf(stderr, "error: missing required option '--timeout'\n");
        usage();
    }

    float notification_timeout_sec = 0.0;
    if(sscanf(timeout_str, "%f", &notification_timeout_sec) != 1) {
        fprintf(stderr, "error: expected timeout to be a floating point number, was: '%s'\n", timeout_str);
        usage();
    }

    const mgl::Color icon_color = parse_hex_color(icon_color_str ? icon_color_str : "FFFFFF");
    const mgl::Color bg_color = parse_hex_color(bg_color_str ? bg_color_str : "76b900");

    mgl::Init init;
    Display *display = (Display*)mgl_get_context()->connection;
    const bool wayland = is_xwayland(display);
    const bool use_transparency = wayland;

    mgl::Window::CreateParams window_create_params;
    window_create_params.size = { 32, 32 };
    window_create_params.min_size = window_create_params.size;
    window_create_params.max_size = window_create_params.size;
    window_create_params.hidden = true;
    window_create_params.override_redirect = true;
    window_create_params.support_alpha = true;
    window_create_params.background_color = use_transparency ? mgl::Color(0, 0, 0, 0) : bg_color;
    window_create_params.hide_decorations = true;
    window_create_params.window_type = MGL_WINDOW_TYPE_NOTIFICATION;

    mgl::Window window;
    if(!window.create("gsr notify", window_create_params))
        return 1;

    const mgl_window *win = window.internal_window();
    if(win->num_monitors == 0) {
        fprintf(stderr, "Error: no monitors found\n");
        exit(1);
    }

    // The cursor position is wrong on wayland if an x11 window is not focused. On wayland we instead create a window and get the position where the wayland compositor puts it
    Window x11_cursor_window = None;
    const mgl::vec2i cursor_position = get_cursor_position(display, &x11_cursor_window);
    const mgl::vec2i monitor_position_query_value = (x11_cursor_window || !wayland) ? cursor_position : create_window_get_center_position(display);
    const mgl_monitor *focused_monitor = find_monitor_at_position(window, monitor_position_query_value);

    const std::string noto_sans_bold_filepath = resources_path + "fonts/NotoSans-Bold.ttf";
    mgl::MemoryMappedFile font_file;
    if(!font_file.load(noto_sans_bold_filepath.c_str(), mgl::MemoryMappedFile::LoadOptions{true, false}))
        return 1;

    mgl::Font font;
    if(!font.load_from_file(font_file, focused_monitor->size.y / 60))
        return 1;

    mgl::Texture logo_texture;
    if(icon_filepath) {
        std::string icon_filepath_str = icon_filepath;
        if(icon_filepath_str == "record")
            icon_filepath_str = resources_path + "images/record.png";
        else if(icon_filepath_str == "replay")
            icon_filepath_str = resources_path + "images/replay.png";
        else if(icon_filepath_str == "stream")
            icon_filepath_str = resources_path + "images/stream.png";

        if(!logo_texture.load_from_file(icon_filepath_str.c_str(), {false, false, true})) {
            fprintf(stderr, "Warning: failed to load icon\n");
        }
    }

    const int window_height = focused_monitor->size.y / 13;
    float content_padding_left = (int)(window_height * 0.05f);
    if(content_padding_left < 2.0f)
        content_padding_left = 2.0f;

    mgl::Text text(notification_text, font);
    text.set_color(mgl::Color(255, 255, 255, 0));

    mgl::Sprite logo_sprite;
    float logo_sprite_padding_x = (int)(window_height * 0.5f);
    float padding_between_icon_and_text_x = 0.0f;
    if(logo_texture.is_valid()) {
        logo_sprite.set_texture(&logo_texture);
        logo_sprite.set_color(mgl::Color(icon_color.r, icon_color.g, icon_color.b, 0));
        logo_sprite.set_height((int)(window_height * 0.4f));
        logo_sprite_padding_x = (int)((window_height - logo_sprite.get_size().y) * 0.4f);
        padding_between_icon_and_text_x = (int)(logo_sprite_padding_x * 0.7f);
    }

    unsigned char data = 1; // Prefer not being composed to not reduce display fps on AMD when an application is using 100% of GPU
    XChangeProperty(display, window.get_system_handle(), XInternAtom(display, "_NET_WM_BYPASS_COMPOSITOR", False), XA_CARDINAL, 32, PropModeReplace, &data, 1);

    data = 1;
    XChangeProperty(display, window.get_system_handle(), XInternAtom(display, "GAMESCOPE_EXTERNAL_OVERLAY", False), XA_CARDINAL, 32, PropModeReplace, &data, 1);

    // TODO: Make sure the notification always stays on top. Test with starting the notification and then opening youtube in fullscreen.
    const int window_width = content_padding_left + logo_sprite_padding_x + logo_sprite.get_size().x + padding_between_icon_and_text_x + text.get_bounds().size.x + logo_sprite_padding_x + padding_between_icon_and_text_x;
    const mgl::vec2i window_size{window_width, window_height};
    const mgl::vec2i window_start_position{focused_monitor->pos.x + focused_monitor->size.x - window_size.x, focused_monitor->pos.y + window_size.y};
    window.set_size_limits(window_size, window_size);
    window.set_size(window_size);
    if(!use_transparency)
        set_window_clip_region(display, window.get_system_handle(), {0, 0}, {0, 0});
    window.set_position(window_start_position);
    make_window_click_through(display, window.get_system_handle());
    window.set_visible(true);
    make_window_sticky(display, window.get_system_handle());

    const int slide_window_start_x = focused_monitor->pos.x + focused_monitor->size.x;
    const int slide_window_end_x = focused_monitor->pos.x + focused_monitor->size.x - window_size.x;

    const std::array<StateWithPayload, 9> states_to_execute_in_order = {
        StateWithPayload{State::SLIDE_IN_WINDOW,   0.15},
        StateWithPayload{State::PAUSE,             0.03},
        StateWithPayload{State::SLIDE_IN_CONTENT,  0.10},
        StateWithPayload{State::FADE_IN_CONTENT,   0.20},
        StateWithPayload{State::PAUSE,             notification_timeout_sec},
        StateWithPayload{State::FADE_OUT_CONTENT,  0.001},
        StateWithPayload{State::SLIDE_OUT_CONTENT, 0.10},
        StateWithPayload{State::PAUSE,             0.10},
        StateWithPayload{State::SLIDE_OUT_WINDOW,  0.10},
    };
    int current_state_index = 0;

    mgl::Clock state_timer;

    mgl::Rectangle transparency_bg(window_size.to_vec2f());
    transparency_bg.set_color(bg_color);
    transparency_bg.set_position({(float)window_size.x, 0.0f});

    mgl::Rectangle content_bg(window_size.to_vec2f() - mgl::vec2f(content_padding_left, 0.0f));
    content_bg.set_color(mgl::Color(0, 0, 0));

    const int slide_content_start_x = window_size.x + content_padding_left;
    const int slide_content_end_x = content_padding_left;

    const mgl::vec2f content_bg_start_position{(float)slide_content_start_x, 0.0f};
    content_bg.set_position(content_bg_start_position);

    const float content_start_alpha = 0.0f;
    const float content_end_alpha = 1.0f;

    const auto slide_content_handler = [&](double interpolate_start, double interpolate_end, double interpolation) {
        double new_slide_x = interpolate(interpolate_start, interpolate_end, interpolation);
        content_bg.set_position(mgl::vec2f(new_slide_x, content_bg_start_position.y).floor());
        logo_sprite.set_position((content_bg.get_position() + mgl::vec2f(logo_sprite_padding_x, content_bg.get_size().y * 0.5f - logo_sprite.get_size().y * 0.5f)).floor());
        const float content_space_left_pos_x = logo_sprite.get_position().x + logo_sprite.get_size().x + padding_between_icon_and_text_x;
        //const float content_space_left_x = content_bg.get_size().x - content_space_left_pos_x;
        text.set_position((mgl::vec2f(content_space_left_pos_x, content_bg.get_position().y) + mgl::vec2f(0.0f, content_bg.get_size().y) * 0.5f - mgl::vec2f(0.0f, text.get_bounds().size.y) * 0.5f).floor());
    };

    mgl::Event event;
    while(window.is_open()) {
        while(window.poll_event(event)) {}

        const StateWithPayload current_state = states_to_execute_in_order[current_state_index];
        const double state_elapsed_time_sec = state_timer.get_elapsed_time_seconds();
        const double state_interpolation = min_double(1.0, state_elapsed_time_sec / current_state.time_in_state_sec);

        switch(current_state.state) {
            case State::SLIDE_IN_WINDOW: {
                const double new_slide_x = interpolate(slide_window_start_x, slide_window_end_x, state_interpolation);
                const mgl::vec2i window_clip(std::max(0.0, slide_window_start_x - new_slide_x), window_size.y);
                if(use_transparency) {
                    transparency_bg.set_size(window_clip.floor().to_vec2f());
                } else {
                    set_window_clip_region(display, window.get_system_handle(), {window_size.x - window_clip.x, 0}, window_clip);
                    XFlush(display);
                }
                break;
            }
            case State::SLIDE_IN_CONTENT: {
                slide_content_handler(slide_content_start_x, slide_content_end_x, state_interpolation);
                break;
            }
            case State::FADE_IN_CONTENT: {
                const double new_alpha = interpolate(content_start_alpha, content_end_alpha, state_interpolation);
                text.set_color(mgl::Color(255, 255, 255, new_alpha * 255.0f));
                logo_sprite.set_color(mgl::Color(icon_color.r, icon_color.g, icon_color.b, new_alpha * 255.0f));
                break;
            }
            case State::FADE_OUT_CONTENT: {
                const double new_alpha = interpolate(content_end_alpha, content_start_alpha, state_interpolation);
                text.set_color(mgl::Color(255, 255, 255, new_alpha * 255.0f));
                logo_sprite.set_color(mgl::Color(icon_color.r, icon_color.g, icon_color.b, new_alpha * 255.0f));
                break;
            }
            case State::SLIDE_OUT_CONTENT: {
                slide_content_handler(slide_content_end_x, slide_content_start_x, state_interpolation);
                break;
            }
            case State::SLIDE_OUT_WINDOW: {
                const double new_slide_x = interpolate(slide_window_end_x, slide_window_start_x, state_interpolation);
                const mgl::vec2i window_clip(std::max(0.0, slide_window_start_x - new_slide_x), window_size.y);
                if(use_transparency) {
                    transparency_bg.set_size(window_clip.floor().to_vec2f());
                } else {
                    set_window_clip_region(display, window.get_system_handle(), {window_size.x - window_clip.x, 0}, window_clip);
                    XFlush(display);
                }
                break;
            }
            case State::PAUSE: {
                break;
            }
        }

        if(use_transparency) {
            window.clear(mgl::Color(0, 0, 0, 0));
            transparency_bg.set_position(mgl::vec2f(window_size.x - transparency_bg.get_size().x, 0.0f));
            window.draw(transparency_bg);
        } else {
            window.clear(bg_color);
        }
        window.draw(content_bg);
        window.draw(logo_sprite);
        window.draw(text);
        window.display();

        if(state_interpolation >= 1.0) {
            state_timer.restart();
            ++current_state_index;
            if(current_state_index >= (int)states_to_execute_in_order.size())
                window.close();
        }
    }
}
