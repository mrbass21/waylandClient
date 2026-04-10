#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

static int set_cloexec_or_close(int fd) {
    long flags;

    if(fd == -1) {
        return -1;
    }

    flags = fcntl(fd, F_GETFD);
    if(flags == -1) {
        goto err;
    }

    if(fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
        goto err;
    }

    return fd;

err:
    close(fd);
    return -1;
}

static int create_tmpfile_cloexec(char *tmpname) {
    int fd;

#ifdef HAVE_MKOSTEMP
    fd = mkostemp(tmpname, O_CLOEXEC);
    if (fd >=0) {
        unlink(tmpname);
    }
#else
    fd = mkstemp(tmpname);
    if(fd >= 0) {
        fd = set_cloexec_or_close(fd);
        unlink(tmpname);
    }
#endif

    return fd;
}

/*
 * Create a new, unique, anonymous file of the given size, and
 * return the file descriptor for it. The file descriptor is set
 * CLOEXEC. The file is immediately suitable for mmap()'ing
 * the given size at offset zero.
 *
 * The file should not have a permanent backing store like a disk,
 * but may have if XDG_RUNTIME_DIR is not properly implemented in OS.
 *
 * The file name is deleted from the file system.
 *
 * The file is suitable for buffer sharing between processes by
 * transmitting the file descriptor over Unix sockets using the
 * SCM_RIGHTS methods.
 */

 int os_create_anonymous_file(off_t size) {
    static const char template[] = "/shm-shared-XXXXXX";
    const char *path;
    char *name;
    int fd;
    int ret;

    path = getenv("XDG_RUNTIME_DIR");
    if(!path) {
        errno = ENOENT;
        return -1;
    }

    // Calculate required buffer size: strlen(path) + template length + null terminator
    size_t required_size = strlen(path) + strlen(template) + 1;
    name = malloc(required_size);
    if(!name) {
        return -1;
    }

    // Use snprintf for safe string formatting
    ret = snprintf(name, required_size, "%s%s", path, template);
    if (ret < 0 || ret >= (int)required_size) {
        free(name);
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = create_tmpfile_cloexec(name);
    free(name);

    if(fd < 0) {
        return -1;
    }

    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }

    return fd;
 }
