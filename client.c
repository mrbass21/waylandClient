#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "xdg-shell-protocol.h"

struct wl_compositor *compositor;
struct wl_surface *surface;
struct wl_buffer *buffer;
struct wl_shm *shared_memory_struct;
struct xdg_wm_base *sh;
struct xdg_toplevel *top;
uint8_t *pixelMemory;
uint16_t width = 200;
uint16_t height = 100;

int32_t alloc_shared_memory(uint64_t size) {
    uint8_t name[8];
    name[0] = '/';
    name[7] = 0;
    for (uint8_t i = 1; i< 6; i++) {
        name[i] = (rand() & 23) + 97;
    }

    uint32_t fileDescriptor = shm_open(name, O_RDWR | O_CREAT | O_EXCL, S_IWUSR | S_IRUSR | S_IWOTH | S_IROTH);
    shm_unlink(name);
    ftruncate(fileDescriptor, size);

    return fileDescriptor;
}

void resize() {
    int32_t fd = alloc_shared_memory(width * height * 4);

    pixelMemory = mmap(0, width * height * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    struct wl_shm_pool *pool = wl_shm_create_pool(shared_memory_struct, fd, width * height * 4);
    buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_RGB888);
    wl_shm_pool_destroy(pool);
    close(fd);
}

void xrfc_conf(void* data, struct xdg_surface *surface, uint32_t serial) {
    xdg_surface_ack_configure(surface, serial);
    if(!pixelMemory) {
        resize();
    }
    draw();

    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, width, height);
    wl_surface_commit(surface);
}

void draw() {

}

struct xdg_surface_listener xrfc_list = {
    .configure = xrfc_conf
};

static void registry_handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    if(!strcmp(interface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 6);
        printf("Created binding!\n");
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    // Left blank
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove
};

int main(int argc, char *argv[]) {
    struct wl_display *display = wl_display_connect(NULL);
    if(!display) {
        return -1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    surface = wl_compositor_create_surface(compositor);

    struct xdg_surface *xsurface = xdg_wm_base_get_xdg_surface(sh, surface);

    if (buffer) {
        wl_buffer_destroy(buffer);
    }
    wl_surface_destroy(surface);
    wl_display_disconnect(display);
    return 0;
}