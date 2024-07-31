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

#include <X11/Xlib.h>

extern "C" {
#include <mgl/mgl.h>
}

static const mgl::Color bg_color(118, 185, 0);

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

static float max_float(float a, float b) {
    return a > b ? a : b;
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

static Bool make_window_always_on_top(Display* display, Window window) {
    Atom net_wm_state_above_atom = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
    if(!net_wm_state_above_atom) {
        fprintf(stderr, "Error: failed to find atom _NET_WM_STATE_ABOVE\n");
        return False;
    }
    
    return set_window_wm_state(display, window, net_wm_state_above_atom);
}

static Bool make_window_sticky(Display* display, Window window) {
    Atom net_wm_state_sticky_atom = XInternAtom(display, "_NET_WM_STATE_STICKY", False);
    if(!net_wm_state_sticky_atom) {
        fprintf(stderr, "Error: failed to find atom _NET_WM_STATE_STICKY\n");
        return False;
    }
    
    return set_window_wm_state(display, window, net_wm_state_sticky_atom);
}

int main(int argc, char **argv) {
    if(argc != 2) {
        fprintf(stderr, "usage: gpu-screen-recorder-notification <title>\n");
        exit(1);
    }

    const char *notification_title = argv[1];
    const double notification_timeout_sec = 3.0;

    mgl::Init init;

    mgl::Window::CreateParams window_create_params;
    window_create_params.size = { 1280, 720 };
    window_create_params.min_size = window_create_params.size;
    window_create_params.max_size = window_create_params.max_size;
    window_create_params.hidden = true;
    window_create_params.override_redirect = true;
    window_create_params.background_color = bg_color;
    window_create_params.hide_decorations = true;
    window_create_params.window_type = MGL_WINDOW_TYPE_DIALOG;

    mgl::Window window;
    if(!window.create("GPU Screen Recorder Notification", window_create_params))
        return 1;

    mgl_window *win = window.internal_window();
    if(win->num_monitors == 0) {
        fprintf(stderr, "Error: no monitors found\n");
        exit(1);
    }

    mgl::MemoryMappedFile font_file;
    if(!font_file.load("/usr/share/fonts/noto/NotoSans-Bold.ttf", mgl::MemoryMappedFile::LoadOptions{true, false}))
        return 1;

    mgl::Font font;
    if(!font.load_from_file(font_file, win->monitors[0].size.y / 60))
        return 1;

    mgl::Texture record_texture;
    if(!record_texture.load_from_file("images/record.png"))
        return 1;

    const int window_height = win->monitors[0].size.y / 12;
    const float content_padding_left = 10.0f;//max_float(5.0f, (float)window_size.x / 30.0f);

    mgl::Text content_title(notification_title, font);
    content_title.set_color(mgl::Color(255, 255, 255, 0));

    mgl::Sprite content_record_sprite(&record_texture);
    content_record_sprite.set_color(mgl::Color(118, 185, 0, 0));
    content_record_sprite.set_height((int)(window_height * 0.4f));
    const float content_record_sprite_padding_x = (int)((window_height - content_record_sprite.get_size().y) * 0.4f);
    const float padding_between_icon_and_text_x = (int)(content_record_sprite_padding_x * 0.7f);

    // TODO: Use the monitor that the cursor is on or the focused window is on
    const int window_width = content_padding_left + content_record_sprite_padding_x + content_record_sprite.get_size().x + padding_between_icon_and_text_x + content_title.get_bounds().size.x + content_record_sprite_padding_x + padding_between_icon_and_text_x + content_padding_left;
    const mgl::vec2i window_size{window_width, window_height};
    const mgl::vec2i window_start_position{win->monitors[0].size.x, window_size.y};
    window.set_size_limits(window_size, window_size);
    window.set_size(window_size);
    window.set_position(window_start_position);
    window.set_visible(true);

    Display *display = (Display*)mgl_get_context()->connection;
    make_window_always_on_top(display, window.get_system_handle());
    make_window_sticky(display, window.get_system_handle());

    const int slide_window_start_x = win->monitors[0].size.x;
    const int slide_window_end_x = win->monitors[0].size.x - window_size.x;

    const std::array<StateWithPayload, 9> states_to_execute_in_order = {
        StateWithPayload{State::SLIDE_IN_WINDOW,   0.15},
        StateWithPayload{State::PAUSE,             0.05},
        StateWithPayload{State::SLIDE_IN_CONTENT,  0.10},
        StateWithPayload{State::FADE_IN_CONTENT,   0.50},
        StateWithPayload{State::PAUSE,             notification_timeout_sec},
        StateWithPayload{State::FADE_OUT_CONTENT,  0.001},
        StateWithPayload{State::SLIDE_OUT_CONTENT, 0.15},
        StateWithPayload{State::PAUSE,             0.10},
        StateWithPayload{State::SLIDE_OUT_WINDOW,  0.15},
    };
    int current_state_index = 0;

    mgl::Clock state_timer;

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
        content_record_sprite.set_position((content_bg.get_position() + mgl::vec2f(content_record_sprite_padding_x, content_bg.get_size().y * 0.5f - content_record_sprite.get_size().y * 0.5f)).floor());
        const float content_space_left_pos_x = content_record_sprite.get_position().x + content_record_sprite.get_size().x + padding_between_icon_and_text_x;
        //const float content_space_left_x = content_bg.get_size().x - content_space_left_pos_x;
        content_title.set_position((mgl::vec2f(content_space_left_pos_x, content_bg.get_position().y) + mgl::vec2f(0.0f, content_bg.get_size().y) * 0.5f - mgl::vec2f(0.0f, content_title.get_bounds().size.y) * 0.5f).floor());
    };

    mgl::Event event;
    while(window.is_open()) {
        while(window.poll_event(event)) {}

        const StateWithPayload current_state = states_to_execute_in_order[current_state_index];
        const double state_elapsed_time_sec = state_timer.get_elapsed_time_seconds();
        const double state_interpolation = min_double(1.0, state_elapsed_time_sec / current_state.time_in_state_sec);

        switch(current_state.state) {
            case State::SLIDE_IN_WINDOW: {
                double new_slide_x = interpolate(slide_window_start_x, slide_window_end_x, state_interpolation);
                window.set_position(mgl::vec2i(new_slide_x, window_start_position.y));
                break;
            }
            case State::SLIDE_IN_CONTENT: {
                slide_content_handler(slide_content_start_x, slide_content_end_x, state_interpolation);
                break;
            }
            case State::FADE_IN_CONTENT: {
                double new_alpha = interpolate(content_start_alpha, content_end_alpha, state_interpolation);
                content_title.set_color(mgl::Color(255, 255, 255, new_alpha * 255.0f));
                content_record_sprite.set_color(mgl::Color(118, 185, 0, new_alpha * 255.0f));
                break;
            }
            case State::FADE_OUT_CONTENT: {
                double new_alpha = interpolate(content_end_alpha, content_start_alpha, state_interpolation);
                content_title.set_color(mgl::Color(255, 255, 255, new_alpha * 255.0f));
                content_record_sprite.set_color(mgl::Color(118, 185, 0, new_alpha * 255.0f));
                break;
            }
            case State::SLIDE_OUT_CONTENT: {
                slide_content_handler(slide_content_end_x, slide_content_start_x, state_interpolation);
                break;
            }
            case State::SLIDE_OUT_WINDOW: {
                double new_slide_x = interpolate(slide_window_end_x, slide_window_start_x, state_interpolation);
                window.set_position(mgl::vec2i(new_slide_x, window_start_position.y));
                break;
            }
            case State::PAUSE: {
                break;
            }
        }

        window.clear(bg_color);
        window.draw(content_bg);
        window.draw(content_record_sprite);
        window.draw(content_title);
        window.display();

        if(state_interpolation >= 1.0) {
            state_timer.restart();
            ++current_state_index;
            if(current_state_index >= (int)states_to_execute_in_order.size())
                window.close();
        }
    }
}
