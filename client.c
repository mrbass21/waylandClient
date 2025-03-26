#include <stdio.h>
#include <stdlib.h>

#include <wayland-client.h>


struct wl_display *display = NULL;

int main(int argc, char *argv[]) {
    display = wl_display_connect(NULL);
    if(display == NULL) {
        fprintf(stderr, "Unable to connect to a Wayland display!\n");
        exit(1);
    }

    printf("Connected to a display!\n");

    wl_display_disconnect(display);
    printf("Disconnected from the display!\n");

    return 0;
}