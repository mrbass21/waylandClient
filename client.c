#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include "xdg-decoration.h"
#include "xdg-shell-protocol.h"

#include "shm.c"
#include <sys/mman.h>

static struct wl_shm *shm = NULL;
static struct wl_buffer *buffer = NULL;
void *shm_data;

static struct wl_display *display = NULL;
static struct wl_registry *registry = NULL;
static struct wl_compositor *compositor = NULL;
static struct wl_surface *surface = NULL;

static struct xdg_wm_base *xdg_wm_base = NULL;
static struct xdg_surface *xdg_surface = NULL;
static struct xdg_toplevel *xdg_toplevel = NULL;
static struct zxdg_decoration_manager_v1 *decoration_manager = NULL;

static int running = 1;

static void xdg_wm_base_ping_handler(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping_handler
};

void redraw_frame() {
    fprintf(stdout, "Redrawing frame!\n");    
    uint32_t *pixel = shm_data;
    for(int i = 0; i < 480 * 360; ++i) {
        *pixel = 0xFFFF00FF;
        ++pixel;
    }
}

static void xdg_surface_configure_handler(void *data, struct xdg_surface *xdg_surf, uint32_t serial) {
    xdg_surface_ack_configure(xdg_surf, serial);

    redraw_frame();

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure_handler
};

static void xdg_toplevel_configure_handler(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height, struct wl_array *states) {
    fprintf(stderr, "XDG toplevel configure: %dx%d\n", width, height);
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

static const struct wl_shm_listener shm_listener = {
    .format = shm_format_handler
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
    }
}

static void global_registry_remove_handler(void *data, struct wl_registry *registry, uint32_t id) {
    printf("Got a registry losing event for <%d>\n", id);
}

const static struct wl_registry_listener registry_listener = {
    .global = global_registry_handler,
    .global_remove = global_registry_remove_handler
};

static struct wl_buffer* create_buffer(int width, int height) {
    struct wl_shm_pool *pool;
    int stride = width * 4;
    int size = stride * height;
    int fd;
    struct wl_buffer *buff;

    fd = os_create_anonumous_file(size);
    if (fd < 0) {
        fprintf(stderr, "Failed to create a buffer. size: %d\n", size);
        exit(1);
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

    return buff;
}

int main(int argc, char *argv[]) {
    display = wl_display_connect(NULL);
    if(display == NULL) {
        fprintf(stderr, "Unable to connect to a Wayland display!\n");
        exit(1);
    }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    buffer = create_buffer(480, 360);

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

    // Trigger a configure event
    wl_surface_commit(surface);

    while (wl_display_dispatch(display) != -1 && running == 1) {
        ;
    }

    wl_surface_destroy(surface);

    wl_display_disconnect(display);
    printf("Disconnected from the display!\n");

    return 0;
}
