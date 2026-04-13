#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include "xdg-decoration.h"
#include "xdg-shell-protocol.h"

#include "shm.c"
#include <sys/mman.h>
#include <xkbcommon/xkbcommon.h>
#include <libudev.h>
#include <fcntl.h>
#include <alsa/asoundlib.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <linux/joystick.h>
#include <poll.h>
#include <sys/time.h>

static struct wl_shm *shm = NULL;
static struct wl_buffer *buffer = NULL;
static struct wl_shm_pool *pool = NULL;
static int shm_fd = -1;
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

#define MAX_WIDTH 1920
#define MAX_HEIGHT 1080

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

static void initialize_buffer_pool() {
    if (pool != NULL) {
        return; // Already initialized
    }
    
    int max_stride = MAX_WIDTH * 4;
    int max_size = max_stride * MAX_HEIGHT;
    if(max_size > SIZE_MAX) {
        exit(1);
    }
    
    shm_fd = os_create_anonymous_file(max_size);
    if (shm_fd < 0) {
        fprintf(stderr, "Failed to create buffer pool. size: %d\n", max_size);
        exit(1);
    }
    
    shm_data = mmap(NULL, max_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_data == MAP_FAILED) {
        fprintf(stderr, "mmap failed!\n");
        close(shm_fd);
        shm_fd = -1;
        exit(1);
    }
    
    pool = wl_shm_create_pool(shm, shm_fd, max_size);
    current_buffer_size = max_size;
}

static struct wl_buffer* create_buffer(int width, int height) {
    assert(width > 0 && height > 0);
    printf("Create Buffer! %dx%d\n", width, height);
    
    initialize_buffer_pool();
    
    int stride = width * 4;
    struct wl_buffer *buff;
    
    buff = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    
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

    struct wl_buffer *old_buffer = buffer;
    buffer = create_buffer(window_width, window_height);

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);

    if(old_buffer != NULL) {
        wl_buffer_destroy(old_buffer);
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure_handler
};

static void xdg_toplevel_configure_handler(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states) {

    fprintf(stderr, "XDG toplevel configure: %dx%d\n", width, height);
    if (width <= 0 || width > MAX_WIDTH) width = MIN_WIDTH;
    if (height <= 0 || height > MAX_HEIGHT) height = MIN_HEIGHT;
    window_height = height;
    window_width = width;
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
    assert(data != NULL);
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

// Audio system state
static struct {
    snd_pcm_t *pcm_handle;
    unsigned int sample_rate;
    int16_t *buffer;
    unsigned int buffer_frames;
    uint64_t sample_position;  // Track position for continuous waveforms
    int device_ready;          // Flag to track if device is initialized
} audio_state = {0};

// Initialize audio system - call this once at startup
static int init_audio() {
    int err;
    snd_pcm_hw_params_t *params;
    snd_pcm_sw_params_t *swparams;
    
    audio_state.sample_rate = 48000;
    audio_state.buffer_frames = 2048;
    audio_state.device_ready = 0;
    
    // Open PCM device for playback in BLOCKING mode (not non-blocking)
    err = snd_pcm_open(&audio_state.pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "Failed to open PCM device: %s\n", snd_strerror(err));
        return -1;
    }
    
    // Allocate hardware parameter structure
    snd_pcm_hw_params_alloca(&params);
    
    // Init hardware parameter structure
    err = snd_pcm_hw_params_any(audio_state.pcm_handle, params);
    if (err < 0) {
        fprintf(stderr, "Cannot configure PCM device: %s\n", snd_strerror(err));
        snd_pcm_close(audio_state.pcm_handle);
        return -1;
    }
    
    // Set access type (interleaved)
    err = snd_pcm_hw_params_set_access(audio_state.pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        fprintf(stderr, "Failed to set access type: %s\n", snd_strerror(err));
        snd_pcm_close(audio_state.pcm_handle);
        return -1;
    }
    
    // Set sample format
    err = snd_pcm_hw_params_set_format(audio_state.pcm_handle, params, SND_PCM_FORMAT_S16_LE);
    if (err < 0) {
        fprintf(stderr, "Failed to set format: %s\n", snd_strerror(err));
        snd_pcm_close(audio_state.pcm_handle);
        return -1;
    }
    
    // Set number of channels (mono)
    err = snd_pcm_hw_params_set_channels(audio_state.pcm_handle, params, 1);
    if (err < 0) {
        fprintf(stderr, "Failed to set channels: %s\n", snd_strerror(err));
        snd_pcm_close(audio_state.pcm_handle);
        return -1;
    }
    
    // Set sample rate
    unsigned int rate_actual = audio_state.sample_rate;
    err = snd_pcm_hw_params_set_rate_near(audio_state.pcm_handle, params, &rate_actual, NULL);
    if (err < 0) {
        fprintf(stderr, "Failed to set sample rate: %s\n", snd_strerror(err));
        snd_pcm_close(audio_state.pcm_handle);
        return -1;
    }
    audio_state.sample_rate = rate_actual;
    
    // Set period and buffer size for stable playback
    snd_pcm_uframes_t period_size = 512;
    snd_pcm_uframes_t buffer_size = 4096;
    
    snd_pcm_hw_params_set_period_size_near(audio_state.pcm_handle, params, &period_size, NULL);
    snd_pcm_hw_params_set_buffer_size_near(audio_state.pcm_handle, params, &buffer_size);
    
    // Apply parameters
    err = snd_pcm_hw_params(audio_state.pcm_handle, params);
    if (err < 0) {
        fprintf(stderr, "Failed to set hw params: %s\n", snd_strerror(err));
        snd_pcm_close(audio_state.pcm_handle);
        return -1;
    }
    
    // Set software parameters
    snd_pcm_sw_params_alloca(&swparams);
    snd_pcm_sw_params_current(audio_state.pcm_handle, swparams);
    
    // Start playback when buffer has data
    snd_pcm_sw_params_set_start_threshold(audio_state.pcm_handle, swparams, 1);
    snd_pcm_sw_params(audio_state.pcm_handle, swparams);
    
    // Prepare PCM for playback
    snd_pcm_prepare(audio_state.pcm_handle);
    
    // Allocate buffer for audio frames
    audio_state.buffer = malloc(audio_state.buffer_frames * sizeof(int16_t));
    if (!audio_state.buffer) {
        fprintf(stderr, "Failed to allocate audio buffer\n");
        snd_pcm_close(audio_state.pcm_handle);
        return -1;
    }
    
    audio_state.device_ready = 1;
    fprintf(stderr, "Audio initialized: %d Hz, 16-bit mono, period=%lu, buffer=%lu\n", 
            audio_state.sample_rate, period_size, buffer_size);
    
    // Pre-fill buffer to avoid initial underrun
    int16_t initial_buffer[4096];
    for (int i = 0; i < 4096; i++) {
        initial_buffer[i] = 0;  // Silent fill
    }
    snd_pcm_writei(audio_state.pcm_handle, initial_buffer, 4096);
    
    return 0;
}

// Get audio bytes for the given deltaTime
// frequency_hz: frequency of sine wave to generate
// delta_time_seconds: time elapsed since last call
// Returns: number of samples written to audio device, or -1 on error
static int get_sound_bytes(int frequency_hz, float delta_time_seconds) {
    if (!audio_state.device_ready || !audio_state.pcm_handle) {
        return -1;
    }
    
    // Calculate how many frames to generate for this delta time
    unsigned int frames_to_generate = (unsigned int)(delta_time_seconds * audio_state.sample_rate);
    if (frames_to_generate == 0) return 0;
    
    // Ensure we don't overflow buffer
    if (frames_to_generate > audio_state.buffer_frames) {
        frames_to_generate = audio_state.buffer_frames;
    }
    
    // Generate sine wave samples
    for (unsigned int i = 0; i < frames_to_generate; i++) {
        float angle = 2.0f * M_PI * frequency_hz * (audio_state.sample_position + i) / (float)audio_state.sample_rate;
        float sample = sinf(angle);
        // Convert to 16-bit signed integer with 0.7 factor to avoid clipping
        audio_state.buffer[i] = (int16_t)(sample * 32767.0f * 0.7f);
    }
    
    // Try to write, but don't block long. If buffer is full, skip this frame.
    snd_pcm_sframes_t frames_out = snd_pcm_writei(audio_state.pcm_handle, audio_state.buffer, frames_to_generate);
    
    if (frames_out < 0) {
        // Handle underrun/error
        if (frames_out == -EPIPE || frames_out == -ESTRPIPE) {
            // Underrun - quietly recover without output spam
            snd_pcm_prepare(audio_state.pcm_handle);
            
            // Retry once
            frames_out = snd_pcm_writei(audio_state.pcm_handle, audio_state.buffer, frames_to_generate);
            if (frames_out < 0) {
                return 0;
            }
        } else if (frames_out == -EAGAIN || frames_out == -EWOULDBLOCK) {
            // Buffer temporarily full, skip this frame (non-blocking would return this)
            return 0;
        } else {
            fprintf(stderr, "PCM write error: %s\n", snd_strerror(frames_out));
            return 0;
        }
    }
    
    if (frames_out > 0) {
        audio_state.sample_position += frames_out;
    }
    
    return frames_out;
}

// Cleanup audio system - call this on shutdown
static void cleanup_audio() {
    if (audio_state.pcm_handle) {
        snd_pcm_drain(audio_state.pcm_handle);
        snd_pcm_close(audio_state.pcm_handle);
        audio_state.pcm_handle = NULL;
    }
    if (audio_state.buffer) {
        free(audio_state.buffer);
        audio_state.buffer = NULL;
    }
}

// Test function - generates varying tones
static int test_get_sound_bytes(float delta_time_seconds) {
    static float test_timer = 0.0f;
    test_timer += delta_time_seconds;
    
    // Cycle through different frequencies every second
    int frequencies[] = {440, 550, 660, 880};  // A notes
    int freq_index = ((int)test_timer) % 4;
    int frequency = frequencies[freq_index];
    
    return get_sound_bytes(frequency, delta_time_seconds);
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

    // Initialize audio system
    if (init_audio() < 0) {
        fprintf(stderr, "Failed to initialize audio\n");
        exit(1);
    }

    #define TARGET_FPS 60
    #define FRAME_TIME_MS (1000 / TARGET_FPS)  // 16ms for 60 FPS
    #define FRAME_TIME_US (FRAME_TIME_MS * 1000)

    while (running == 1) {
        uint64_t frame_start = 0;
        struct timeval tv;
        gettimeofday(&tv, NULL);
        frame_start = tv.tv_sec * 1000000LL + tv.tv_usec;
        
        // Update game state and render
        redraw_frame();
        
        // Generate audio for this frame (test implementation)
        float delta_time = FRAME_TIME_MS / 1000.0f;  // Convert to seconds
        test_get_sound_bytes(delta_time);  // Cycles through tones every second
        
        // Commit the buffer to Wayland
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage(surface, 0, 0, window_width, window_height);
        wl_surface_commit(surface);
        
        // Flush to ensure requests reach compositor
        wl_display_flush(display);
        
        // Process gamepad input
        process_gamepad_input();
        
        // Check for events without blocking (timeout 0)
        struct pollfd fds[1];
        fds[0].fd = wl_display_get_fd(display);
        fds[0].events = POLLIN;
        
        if (poll(fds, 1, 0) > 0 && fds[0].revents & POLLIN) {
            if (wl_display_dispatch(display) == -1) {
                fprintf(stderr, "Wayland dispatch error!\n");
                break;
            }
        }
        
        // Sleep for remaining frame time
        uint64_t frame_end;
        gettimeofday(&tv, NULL);
        frame_end = tv.tv_sec * 1000000LL + tv.tv_usec;
        
        uint64_t elapsed = frame_end - frame_start;
        if (elapsed < FRAME_TIME_US) {
            usleep(FRAME_TIME_US - elapsed);
        }
    }

    cleanup_audio();

    close_gamepads();

    wl_surface_destroy(surface);
    wl_seat_destroy(wayland_seat);
    zxdg_decoration_manager_v1_destroy(decoration_manager);
    xdg_toplevel_destroy(xdg_toplevel);
    xdg_surface_destroy(xdg_surface);

    wl_display_disconnect(display);
}