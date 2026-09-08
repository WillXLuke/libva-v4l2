// SPDX-License-Identifier: MIT
// Hardware fallback test only:
// cc -shared -fPIC tests/encoder/reject-coherent.c -ldl \
//    $(pkg-config --cflags libdrm) -o reject-coherent.so
// LD_PRELOAD=$PWD/reject-coherent.so ./api-smoke
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <xf86drm.h>
#include <msm_drm.h>

#ifndef MSM_BO_CACHED_COHERENT
#define MSM_BO_CACHED_COHERENT 0x080000
#endif

int drmIoctl(int fd, unsigned long request, void *arg) {
    if (request == DRM_IOCTL_MSM_GEM_NEW &&
        (((struct drm_msm_gem_new *)arg)->flags & MSM_BO_CACHED_COHERENT)) {
        errno = EINVAL;
        return -1;
    }
    int (*next)(int, unsigned long, void *) = dlsym(RTLD_NEXT, "drmIoctl");
    if (!next) {
        errno = ENOSYS;
        return -1;
    }
    return next(fd, request, arg);
}
