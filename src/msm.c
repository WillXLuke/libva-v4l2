// SPDX-License-Identifier: MIT
// Keep the MSM uAPI in C: older libdrm headers use the C++ keyword "or".
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <msm_drm.h>

int irisva_msm_allocate(int drm_fd, uint64_t size) {
    struct drm_msm_gem_new allocation = {.size = size, .flags = MSM_BO_WC};
    if (drmIoctl(drm_fd, DRM_IOCTL_MSM_GEM_NEW, &allocation) < 0)
        return -1;
    int prime_fd = -1;
    int result = drmPrimeHandleToFD(drm_fd, allocation.handle, DRM_CLOEXEC | DRM_RDWR, &prime_fd);
    int saved_errno = errno;
    struct drm_gem_close close_handle = {.handle = allocation.handle};
    if (drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &close_handle) < 0) {
        if (prime_fd >= 0)
            close(prime_fd);
        return -1;
    }
    if (result < 0) {
        errno = saved_errno;
        return -1;
    }
    return prime_fd;
}
