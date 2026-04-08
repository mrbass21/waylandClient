#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include "xdg-decoration.h"
#include "xdg-shell-protocol.h"

#include "shm.c"
#include <sys/mman.h>
#include <xkbcommon/xkbcommon.h>
#include <libudev.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/joystick.h>
#include <poll.h>

static struct wl_shm *shm = NULL;
static struct wl_buffer *buffer = NULL;
static uint64_t current_buffer_size = 0;
void *shm_data;

static struct wl_display *display = NULL;
static struct wl_registry *registry = NULL;
static struct wl_compositor *compositor = NULL;
static struct wl_surface *surface = NULL;

static struct xdg_wm_base *xdg_wm_base = NULL;
static struct xdg_surface *xdg_surface = NULL;
static struct xdg_toplevel *xdg_toplevel = NULL;
static struct zxdg_decoration_manager_v1 *decoration_manager = NULL;
static struct wl_seat *wayland_seat = NULL;
static struct wl_keyboard *keyboard = NULL;
static bool keyboardConnected = false;

#define MAX_GAMEPADS 4
static int gamepad_fds[MAX_GAMEPADS] = {-1, -1, -1, -1};
static int gamepad_count_active = 0;

#define BACKGROUND_COLOR 0xFF00FF00; //ARGB

#define MIN_WIDTH 480
#define MIN_HEIGHT 320

static uint32_t window_width = MIN_WIDTH;
static uint32_t window_height = MIN_HEIGHT;

static int running = 1;
static int frame_ready = 0;

static struct client_keyboard_state {
    struct xkb_context *context;
    struct xkb_keymap *keymap;
    struct xkb_state *state;
    int32_t repeat_rate;      // characters per second (0 = disabled)
    int32_t repeat_delay;     // milliseconds before repeat starts
    uint32_t repeat_key;      // key code currently repeating
    uint32_t repeat_start_time; // when the key was pressed
} keyboard_state;

static struct wl_buffer* create_buffer(int width, int height) {
    printf("Create Buffer!\n");
    struct wl_shm_pool *pool;
    int stride = width * 4;
    int size = stride * height;
    int fd;
    static int old_fd = -1;
    struct wl_buffer *buff;

    fd = os_create_anonymous_file(size);
    if (fd < 0) {
        fprintf(stderr, "Failed to create a buffer. size: %d\n", size);
        exit(1);
    }

    if(shm_data != NULL) {
        munmap(shm_data, current_buffer_size);
    }

    if(old_fd > 0) {
        close(old_fd);
    }
    
    shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(shm_data == MAP_FAILED) {
        fprintf(stderr, "mmap failed!\n");
        close(fd);
        exit(1);
    }

    pool = wl_shm_create_pool(shm, fd, size);
    buff = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    current_buffer_size = size;
    old_fd = fd;
    return buff;
}

static void xdg_wm_base_ping_handler(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping_handler
};

void redraw_frame() {
    //fprintf(stdout, "Redrawing frame!\n");    
    uint32_t *pixel = shm_data;
    for(int i = 0; i < window_height * window_width; ++i) {
        *pixel = BACKGROUND_COLOR;
        ++pixel;
    }
}
static void frame_callback_handler(void *data, struct wl_callback *callback, uint32_t time) {
    wl_callback_destroy(callback);
    frame_ready = 1;
}

static const struct wl_callback_listener frame_callback_listener = {
    .done = frame_callback_handler
};

static void xdg_surface_configure_handler(void *data, struct xdg_surface *xdg_surf, uint32_t serial) {
    xdg_surface_ack_configure(xdg_surf, serial);

    if(buffer != NULL) {
        wl_buffer_destroy(buffer);
    }

    buffer = create_buffer(window_width, window_height);

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure_handler
};

static void xdg_toplevel_configure_handler(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states) {
    fprintf(stderr, "XDG toplevel configure: %dx%d\n", width, height);
    window_height = (height >= MIN_HEIGHT) ? height : MIN_HEIGHT;
    window_width = (width >= MIN_WIDTH) ? width : MIN_WIDTH;
}

static void xdg_toplevel_close_handler(void *data, struct xdg_toplevel *toplevel) {
    printf("Close requested!\n");
    running = 0;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .close = xdg_toplevel_close_handler,
    .configure = xdg_toplevel_configure_handler
};

static void shm_format_handler(void *data, struct wl_shm *shm, uint32_t format) {
    fprintf(stderr, "Format %d\n", format);
}

// keyboard handlers
static void keyboard_keymap_handler(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size) {
    struct client_keyboard_state *client_state = data;

    if(format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        fprintf(stderr, "Unknown keymap format: %d\n", format);
        close(fd);
        return;
    }

    char *map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if(map_shm == MAP_FAILED) {
        fprintf(stderr, "Failed to mmap keymap!\n");
        close(fd);
        return;
    }
    client_state->keymap = xkb_keymap_new_from_string(client_state->context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map_shm, size);
    close(fd);
    if(client_state->keymap == NULL) {
        fprintf(stderr, "Failed to compile keymap!\n");
        return;
    }

    struct xkb_state *state = xkb_state_new(client_state->keymap);
    xkb_keymap_unref(client_state->keymap);
    xkb_state_unref(client_state->state);
    client_state->state = state;  
}

static void wl_keyboard_modifiers_handler(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    struct client_keyboard_state *client_state = data;
    xkb_state_update_mask(client_state->state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

static void wl_keyboard_repeat_info_handler(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
    struct client_keyboard_state *client_state = data;
    client_state->repeat_rate = rate;
    client_state->repeat_delay = delay;
    fprintf(stderr, "Key repeat info: rate %d, delay %d\n", rate, delay);
    fprintf(stderr, "Compositor will handle repeats: %s\n", (rate == 0) ? "Yes (via REPEATED state)" : "No (client handles)");
}

static void wl_keyboard_key_handler(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    struct client_keyboard_state *client_state = data;
    char buf[128];
    uint32_t keycode = key + 8; // xkbcommon adds an offset of 8 to the keycodes, so we need to add 8 to get the correct keycode.
    xkb_keysym_t sym = xkb_state_key_get_one_sym(client_state->state, keycode);
    xkb_keysym_get_name(sym, buf, sizeof(buf));
    
    const char *action;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        action = "pressed";
        // Track this key for repeat handling if client is responsible
        if (client_state->repeat_rate > 0) {
            client_state->repeat_key = key;
            client_state->repeat_start_time = time;
        }
    } else if (state == WL_KEYBOARD_KEY_STATE_REPEATED) {
        action = "repeated";
    } else {
        action = "released";
        // Stop tracking this key
        if (client_state->repeat_key == key) {
            client_state->repeat_key = 0;
            client_state->repeat_start_time = 0;
        }
    }
    
    fprintf(stderr, "Key %s: %s (keycode: %d, sym: %d)\n", action, buf, keycode, sym);
    xkb_state_key_get_utf8(client_state->state, keycode, buf, sizeof(buf));
    fprintf(stderr, "UTF-8: %s\n", buf);
}

static void wl_keyboard_enter_handler(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
    fprintf(stderr, "Keyboard entered surface!\n");
}

static void wl_keyboard_leave_handler(void *data, struct wl_keyboard *keyboard, uint32_t serial, struct wl_surface *surface) {
    fprintf(stderr, "Keyboard left surface!\n");
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap_handler,
    .enter = wl_keyboard_enter_handler,
    .leave = wl_keyboard_leave_handler,
    .key = wl_keyboard_key_handler,
    .modifiers = wl_keyboard_modifiers_handler,
    .repeat_info = wl_keyboard_repeat_info_handler
};

// wl_seat handler
static void seat_capabilities_handler(void *data, struct wl_seat *seat, uint32_t capabilities) {
    printf("Seat capabilities changed! %d\n", capabilities);
    struct client_keyboard_state *client_state = data;
    if(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        printf("Keyboard is supported!\n");
        keyboardConnected = true;
        
        keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, client_state);
    } else {
        printf("Keyboard is not supported!\n");
        keyboardConnected = false;
        if(keyboard != NULL) {
            wl_keyboard_release(keyboard);
            keyboard = NULL;
        }
    }
}

static void seat_name_handler(void *data, struct wl_seat *seat, const char *name) {
    printf("Seat name: %s\n", name);
}

static const struct wl_shm_listener shm_listener = {
    .format = shm_format_handler
};

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities_handler,
    .name = seat_name_handler
};

static void global_registry_handler(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
    // printf("Got a registry event for <%s>, id: %d, version: %d\n", interface, id, version);

    if(strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry, id, &wl_compositor_interface, version);
        printf("Set compositor!\n");
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        xdg_wm_base = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);
        printf("Set wm_base!\n");
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
        wl_shm_add_listener(shm, &shm_listener, NULL);
        printf("Set SHM listener!\n");
    } else if( strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        decoration_manager = wl_registry_bind(registry, id, &zxdg_decoration_manager_v1_interface, 1);
    } else if( strcmp(interface, wl_seat_interface.name) == 0) {
        wayland_seat = wl_registry_bind(registry, id, &wl_seat_interface, 7);
        wl_seat_add_listener(wayland_seat, &seat_listener, data);
    }
}

static void global_registry_remove_handler(void *data, struct wl_registry *registry, uint32_t id) {
    printf("Got a registry losing event for <%d>\n", id);
}

const static struct wl_registry_listener registry_listener = {
    .global = global_registry_handler,
    .global_remove = global_registry_remove_handler
};

static void process_gamepad_input() {
    struct js_event event;
    
    for (int i = 0; i < gamepad_count_active; i++) {
        if (gamepad_fds[i] < 0) continue;
        
        // Read all pending events from this gamepad
        while (read(gamepad_fds[i], &event, sizeof(event)) == sizeof(event)) {
            // Ignore initial calibration events
            if (event.type & JS_EVENT_INIT) continue;
            
            if (event.type == JS_EVENT_BUTTON) {
                const char *button_names[] = {
                    "X", "O", "Triangle", "Square",
                    "L1", "R1", "L2", "R2",
                    "Select", "Start", "Center Button", 
                    "L3", "R3", 
                    "DPAD UP", "DPAD DOWN", "DPAD LEFT", "DPAD RIGHT"
                };
                
                if (event.number < sizeof(button_names) / sizeof(button_names[0])) {
                    fprintf(stderr, "Gamepad %d: Button %s %s\n", 
                            i, button_names[event.number], 
                            event.value ? "pressed" : "released");
                }
            } else if (event.type == JS_EVENT_AXIS) {
                const char *axis_names[] = {
                    "Left Stick X", "Left Stick Y", 
                    "Right Stick X", "Right Stick Y",
                    "L2 Analog", "R2 Analog"
                };
                
                if (event.number < sizeof(axis_names) / sizeof(axis_names[0])) {
                    // Only print significant movements to reduce spam
                    if (abs(event.value) > 5000) {
                        fprintf(stderr, "Gamepad %d: %s = %d\n", 
                                i, axis_names[event.number], event.value);
                    }
                }
            }
        }
    }
}

static void close_gamepads() {
    for (int i = 0; i < gamepad_count_active; i++) {
        if (gamepad_fds[i] >= 0) {
            close(gamepad_fds[i]);
            gamepad_fds[i] = -1;
        }
    }
    gamepad_count_active = 0;
}

static void scanForGamepads() {
    fprintf(stderr, "Scanning for gamepads...\n");
    struct udev *udev = udev_new();
    if (!udev) {
        fprintf(stderr, "Failed to create udev context\n");
        return;
    }
    
    struct udev_enumerate *enumerate = udev_enumerate_new(udev);
    if (!enumerate) {
        fprintf(stderr, "Failed to create udev enumerate\n");
        udev_unref(udev);
        return;
    }
    
    // Enumerate input devices with joystick devtype (more reliable)
    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_add_match_property(enumerate, "ID_INPUT_JOYSTICK", "1");
    udev_enumerate_scan_devices(enumerate);
    struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
    struct udev_list_entry *entry;
    
    int gamepad_count = 0;

    udev_list_entry_foreach(entry, devices) {
        const char *path = udev_list_entry_get_name(entry);
        struct udev_device *device = udev_device_new_from_syspath(udev, path);
        if (!device) {
            continue;
        }
        
        const char *devnode = udev_device_get_devnode(device);
        const char *name = udev_device_get_sysattr_value(device, "name");
        
        fprintf(stderr, "Found input device: %s at %s\n", name ? name : "Unknown", devnode ? devnode : "No devnode");
        
        // Check if this is a joystick/gamepad device by devnode pattern
        if (devnode && strstr(devnode, "js")) {
            // Try to get the name from parent device if not available
            const char *gamepad_name = name;
            if (!gamepad_name) {
                struct udev_device *parent = udev_device_get_parent(device);
                if (parent) {
                    gamepad_name = udev_device_get_sysattr_value(parent, "name");
                }
            }
            
            printf("Found gamepad: %s at %s\n", gamepad_name ? gamepad_name : "Unknown Gamepad", devnode);
            
            // Try to open the device
            if (gamepad_count_active < MAX_GAMEPADS) {
                int fd = open(devnode, O_RDONLY | O_NONBLOCK);
                if (fd >= 0) {
                    gamepad_fds[gamepad_count_active] = fd;
                    printf("Opened gamepad device at index %d\n", gamepad_count_active);
                    gamepad_count_active++;
                } else {
                    fprintf(stderr, "Failed to open gamepad device: %s\n", devnode);
                }
            }
            
            gamepad_count++;
        }

        udev_device_unref(device);
    }
    
    if (gamepad_count == 0) {
        fprintf(stderr, "No gamepads found\n");
    }

    udev_enumerate_unref(enumerate);
    udev_unref(udev);
}

int main(int argc, char *argv[]) {
    display = wl_display_connect(NULL);
    if(display == NULL) {
        fprintf(stderr, "Unable to connect to a Wayland display!\n");
        exit(1);
    }

    struct client_keyboard_state keyboard_state = {0};
    keyboard_state.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &keyboard_state);
    wl_display_roundtrip(display);

    buffer = create_buffer(MIN_WIDTH, MIN_HEIGHT);
    current_buffer_size = MIN_WIDTH * MIN_HEIGHT * 4;

    if(compositor == NULL) {
        fprintf(stderr, "Could not find a compositor.\n");
        exit(1);
    }

    if(xdg_wm_base == NULL) {
        fprintf(stderr, "xdg-shell not available\n");
        exit(1);
    }

    if(decoration_manager == NULL) {
        fprintf(stderr, "xdg-decoration not available\n");
        exit(1);
    }

    surface = wl_compositor_create_surface(compositor);
    if(surface == NULL) {
        fprintf(stderr, "Failed to create a surface.\n");
        exit(1);
    }

    xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, surface);
    xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);

    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);

    wl_display_roundtrip(display);

    struct zxdg_toplevel_decoration_v1 *toplevel_decoration =  zxdg_decoration_manager_v1_get_toplevel_decoration(decoration_manager, xdg_toplevel);
    zxdg_toplevel_decoration_v1_set_mode(toplevel_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

    // Trigger a configure event. We do this to set an initial size for the window, and to get the initial configure event with the correct size from the compositor.
    wl_surface_commit(surface);
    wl_display_roundtrip(display);

    // Verify buffer is attached after configure
    if(buffer == NULL) {
        fprintf(stderr, "ERROR: Buffer is NULL after configure!\n");
        exit(1);
    }

    // Find game pads
    scanForGamepads();

    #define TARGET_FPS 60
    #define FRAME_TIME_MS (1000 / TARGET_FPS)  // 16ms for 60 FPS

    while (running == 1) {
        // Update game state and render
        redraw_frame();
        
        // Commit the buffer to Wayland
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage(surface, 0, 0, window_width, window_height);
        wl_surface_commit(surface);
        
        // Flush to ensure requests reach compositor
        wl_display_flush(display);
        
        // Check if buffer needs to be recreated due to window resize
        if (buffer != NULL) {
            uint64_t required_size = (uint64_t)window_width * window_height * 4;
            if (required_size != current_buffer_size) {
                fprintf(stderr, "Buffer size mismatch, recreating buffer: %lu vs %lu\n", required_size, current_buffer_size);
                wl_buffer_destroy(buffer);
                buffer = create_buffer(window_width, window_height);
            }
        }
        
        // Process gamepad input
        process_gamepad_input();
        
        // Wait for events with frame time timeout to maintain FPS while staying responsive
        struct pollfd fds[1];
        fds[0].fd = wl_display_get_fd(display);
        fds[0].events = POLLIN;
        
        int ret = poll(fds, 1, FRAME_TIME_MS);
        if (ret > 0 && fds[0].revents & POLLIN) {
            if (wl_display_dispatch(display) == -1) {
                fprintf(stderr, "Wayland dispatch error!\n");
                break;
            }
        }
    }

    close_gamepads();

    wl_surface_destroy(surface);
    wl_seat_destroy(wayland_seat);
    zxdg_decoration_manager_v1_destroy(decoration_manager);
    xdg_toplevel_destroy(xdg_toplevel);
    xdg_surface_destroy(xdg_surface);

    wl_display_disconnect(display);
}