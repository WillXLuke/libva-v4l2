// SPDX-License-Identifier: MIT
// Test-only shim: hide completed V4L2 buffers to exercise VA sync timeouts.
// cc -shared -fPIC delay-completion.c -ldl -o delay-completion.so
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/videodev2.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/ioctl.h>

int ioctl(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (request == VIDIOC_DQBUF && getenv("IRIS_TEST_BLOCK_DQBUF")) {
        errno = EAGAIN;
        return -1;
    }
    int (*real_ioctl)(int, unsigned long, ...) = dlsym(RTLD_NEXT, "ioctl");
    return real_ioctl(fd, request, arg);
}
