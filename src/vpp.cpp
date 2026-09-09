// SPDX-License-Identifier: MIT
#include "internal.hpp"
#include <climits>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_FASTCV
#include <fastcv/fastcv.h>
#include <fastcv/fastcvExt.h>
#include <dlfcn.h>
#endif

namespace irisva {
#ifdef HAVE_FASTCV
namespace {
struct Library {
    void *handle = nullptr;
    Library(const char *soname, const char *fallback) {
        handle = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
        if (!handle)
            handle = dlopen(fallback, RTLD_NOW | RTLD_LOCAL);
        check(handle, dlerror_message(), VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT);
    }
    static const char *dlerror_message() {
        const char *message = dlerror();
        return message ? message : "FastCV runtime unavailable";
    }
    ~Library() {
        if (handle)
            dlclose(handle);
    }
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;
    template <typename T> T symbol(const char *name) {
        auto pointer = dlsym(handle, name);
        check(pointer, name, VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT);
        return reinterpret_cast<T>(pointer);
    }
};
struct FastcvApi {
    // Keep the RPC library loaded until after FastCV has been unloaded.
    Library rpc{"libcdsprpc.so.1", "libcdsprpc.so"};
    Library fastcv{"libfastcvopt.so.1", "libfastcvopt.so"};
#define FCV_SYMBOL(name) decltype(&name) name##_fn = fastcv.symbol<decltype(&name)>(#name)
    FCV_SYMBOL(fcvSetOperationModeExt);
    FCV_SYMBOL(fcvMemInit);
    FCV_SYMBOL(fcvMemDeInit);
    FCV_SYMBOL(fcvCleanUp);
    FCV_SYMBOL(fcvScaleDownMNu8);
    FCV_SYMBOL(fcvScaleDownMNInterleaveu8);
    FCV_SYMBOL(fcvScaleUpPolyu8);
    FCV_SYMBOL(fcvScaleUpPolyInterleaveu8);
#undef FCV_SYMBOL
    using Register = void (*)(void *, int, int);
    Register register_buf = rpc.symbol<Register>("remote_register_buf");
};
std::shared_ptr<FastcvApi> fastcv_api() {
    // Probe once per driver load. Missing libraries/symbols disable only VPP.
    static auto api = []() -> std::shared_ptr<FastcvApi> {
        try {
            return std::make_shared<FastcvApi>();
        } catch (const Error &e) {
            trace("FastCV VPP unavailable: %s", e.what());
            return {};
        }
    }();
    return api;
}
} // namespace
#endif

bool vpp_supported(VAProfile profile, VAEntrypoint entrypoint) {
#ifdef HAVE_FASTCV
    return profile == VAProfileNone && entrypoint == VAEntrypointVideoProc && bool(fastcv_api());
#else
    (void)profile;
    (void)entrypoint;
    return false;
#endif
}
void vpp_caps(VAProcPipelineCaps &caps) {
    // These are driver-owned arrays, as consumed by FFmpeg and other VA clients.
    // Do not read the incoming structure: clients may pass it uninitialized.
    static VAProcColorStandardType colors[] = {VAProcColorStandardNone, VAProcColorStandardBT601,
                                               VAProcColorStandardBT709, VAProcColorStandardBT2020,
                                               VAProcColorStandardExplicit};
    static uint32_t formats[] = {VA_FOURCC_NV12};
    caps = {};
    caps.input_color_standards = caps.output_color_standards = colors;
    caps.num_input_color_standards = caps.num_output_color_standards =
        sizeof(colors) / sizeof(colors[0]);
    caps.input_pixel_format = caps.output_pixel_format = formats;
    caps.num_input_pixel_formats = caps.num_output_pixel_formats = 1;
    caps.min_input_width = caps.min_output_width = 16;
    caps.min_input_height = caps.min_output_height = 2;
    caps.max_input_width = caps.max_output_width = 8192;
    caps.max_input_height = caps.max_output_height = 8192;
}
VppPicture vpp_picture(const Buffer &buffer, const std::shared_ptr<Surface> &source,
                       const Surface &target) {
    VAProcPipelineParameterBuffer p{};
    std::memcpy(&p, buffer.data.data(), sizeof(p));
    check(!p.num_filters && !p.num_forward_references && !p.num_backward_references &&
              !p.rotation_state && !p.mirror_state && !p.blend_state && !p.num_additional_outputs &&
              !(p.pipeline_flags & ~VA_PROC_PIPELINE_FAST) &&
              !(p.filter_flags & ~VA_FILTER_SCALING_MASK) && !p.input_surface_flag &&
              !p.output_surface_flag && !p.output_hdr_metadata,
          "VPP supports progressive scaling only", VA_STATUS_ERROR_UNIMPLEMENTED);
    const auto &in = p.input_color_properties;
    const auto &out = p.output_color_properties;
    auto matches = [](unsigned a, unsigned b) { return !a || !b || a == b; };
    check(matches(p.surface_color_standard, p.output_color_standard) &&
              matches(in.color_range, out.color_range) &&
              matches(in.chroma_sample_location, out.chroma_sample_location) &&
              matches(in.colour_primaries, out.colour_primaries) &&
              matches(in.transfer_characteristics, out.transfer_characteristics) &&
              matches(in.matrix_coefficients, out.matrix_coefficients),
          "VPP color conversion unsupported", VA_STATUS_ERROR_UNIMPLEMENTED);
    check(!p.output_region ||
              (!p.output_region->x && !p.output_region->y &&
               p.output_region->width == target.width && p.output_region->height == target.height),
          "VPP output must cover the entire surface", VA_STATUS_ERROR_UNIMPLEMENTED);
    VppPicture picture;
    picture.source = source;
    picture.region = p.surface_region ? *p.surface_region
                                      : VARectangle{0, 0, static_cast<uint16_t>(source->width),
                                                    static_cast<uint16_t>(source->height)};
    auto r = picture.region;
    check(r.x >= 0 && r.y >= 0 && r.width && r.height && !(r.x & 15) && !(r.y & 1) &&
              !(r.width & 15) && !(r.height & 1) && !(target.width & 15) && !(target.height & 1) &&
              unsigned(r.x) + r.width <= source->width &&
              unsigned(r.y) + r.height <= source->height,
          "VPP requires 16-pixel widths/aligned crop x and even heights/crop y",
          VA_STATUS_ERROR_INVALID_PARAMETER);
    bool down = target.width <= r.width && target.height <= r.height;
    bool up = target.width >= r.width && target.height >= r.height;
    check((down || up) && uint64_t(target.width) * 20 >= r.width &&
              uint64_t(target.height) * 20 >= r.height,
          "VPP cannot mix up/down scaling axes or downscale beyond 20:1",
          VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED);
    check(source.get() != &target && source->fourcc == VA_FOURCC_NV12 &&
              target.fourcc == VA_FOURCC_NV12,
          "VPP requires distinct NV12 surfaces", VA_STATUS_ERROR_INVALID_SURFACE);
    return picture;
}
#ifdef HAVE_FASTCV
namespace {
std::mutex fastcv_mutex;
// Shared across VA displays and retained until driver unload: FastCV is process-global.
std::shared_ptr<void> fastcv_runtime;
struct Runtime {
    // Registrations and contexts retain the API through DSP cleanup and dlclose.
    std::shared_ptr<FastcvApi> api = fastcv_api();
    Runtime() {
        check(bool(api), "FastCV runtime unavailable", VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT);
        // QDSP selects the CDSP backend in this platform's FastCV build and avoids
        // the per-function CPU preference of FASTCV_OP_PERFORMANCE.
        if (api->fcvSetOperationModeExt_fn(FASTCV_OP_EXT_QDSP) != FASTCV_SUCCESS) {
            api->fcvCleanUp_fn();
            throw Error(VA_STATUS_ERROR_OPERATION_FAILED, "FastCV DSP initialization failed");
        }
        api->fcvMemInit_fn();
    }
    ~Runtime() {
        std::lock_guard<std::mutex> lock(fastcv_mutex);
        api->fcvMemDeInit_fn();
        api->fcvCleanUp_fn();
    }
};
std::shared_ptr<Memory> allocate(const Surface &s) {
    auto m = std::make_shared<Memory>();
    m->origin = MemoryOrigin::DmaHeap;
    m->width = s.width;
    m->height = s.height;
    m->stride = (s.width + 127) & ~127u;
    m->storage_height = (s.height + 31) & ~31u;
    m->size = (size_t(m->stride) * m->storage_height * 3 / 2 + 4095) & ~size_t(4095);
    int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    check(heap >= 0, "open system DMA heap", VA_STATUS_ERROR_ALLOCATION_FAILED);
    dma_heap_allocation_data data{};
    data.len = m->size;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    int result = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data);
    close(heap);
    check(result == 0, "allocate VPP DMA-BUF", VA_STATUS_ERROR_ALLOCATION_FAILED);
    m->fd = data.fd;
    return m;
}
void validate(const Memory &m, const Surface &s) {
    check(m.fd >= 0 && m.fourcc == VA_FOURCC_NV12 && m.size <= INT_MAX && m.width >= s.width &&
              m.height >= s.height && m.stride >= s.width && !(m.stride & 7) &&
              !(m.data_offset & 15) && m.storage_height >= s.height &&
              !((uint64_t(m.stride) * m.storage_height) & 15) &&
              uint64_t(m.data_offset) + uint64_t(m.stride) * (m.storage_height + s.height / 2 - 1) +
                      s.width <=
                  m.size,
          "unsupported FastCV DMA-BUF layout", VA_STATUS_ERROR_INVALID_SURFACE);
}
struct Registration {
    std::shared_ptr<void> runtime;
    void *pointer;
    int size;
    bool active = false;
    Registration(std::shared_ptr<void> r, void *p, int n)
        : runtime(std::move(r)), pointer(p), size(n) {}
    ~Registration() {
        if (active) {
            std::lock_guard<std::mutex> lock(fastcv_mutex);
            std::static_pointer_cast<Runtime>(runtime)->api->register_buf(pointer, size, -1);
        }
    }
};
void register_memory(Memory &m, const std::shared_ptr<void> &runtime) {
    if (m.fastcv_registration)
        return;
    // Allocate ownership before registration so allocation failure cannot leave
    // a stale registration behind. Memory unregisters before unmapping/closing.
    auto registration = std::make_shared<Registration>(runtime, m.map(), static_cast<int>(m.size));
    std::static_pointer_cast<Runtime>(runtime)->api->register_buf(registration->pointer,
                                                                  registration->size, m.fd);
    registration->active = true;
    m.fastcv_registration = std::move(registration);
}
} // namespace
Vpp::Vpp() {
    std::lock_guard<std::mutex> lock(fastcv_mutex);
    runtime_ = fastcv_runtime;
    if (!runtime_) {
        runtime_ = std::make_shared<Runtime>();
        fastcv_runtime = runtime_;
    }
}
void Vpp::scale(const VppPicture &picture, const std::shared_ptr<Surface> &target) {
    auto source = picture.source;
    check(source && source->memory, "VPP source has no storage", VA_STATUS_ERROR_INVALID_SURFACE);
    if (!target->memory)
        target->memory = allocate(*target);
    auto &src = *source->memory;
    auto &dst = *target->memory;
    check(dst.origin != MemoryOrigin::Iris, "VPP cannot overwrite decoder reference storage",
          VA_STATUS_ERROR_INVALID_SURFACE);
    struct stat a{}, b{};
    check(fstat(src.fd, &a) == 0 && fstat(dst.fd, &b) == 0 &&
              (a.st_dev != b.st_dev || a.st_ino != b.st_ino),
          "VPP source and destination DMA-BUF alias", VA_STATUS_ERROR_INVALID_SURFACE);
    validate(src, *source);
    validate(dst, *target);
    wait_surface_access(src, POLLIN);
    wait_surface_access(dst, POLLOUT);
    std::lock_guard<std::mutex> lock(fastcv_mutex);
    auto r = picture.region;
    auto *sy = src.map() + src.data_offset + size_t(r.y) * src.stride + r.x;
    auto *suv =
        src.map() + src.data_offset + size_t(src.storage_height + r.y / 2) * src.stride + r.x;
    auto *dy = dst.map() + dst.data_offset;
    auto *duv = dy + size_t(dst.storage_height) * dst.stride;
    check(!((reinterpret_cast<uintptr_t>(sy) | reinterpret_cast<uintptr_t>(suv) |
             reinterpret_cast<uintptr_t>(dy) | reinterpret_cast<uintptr_t>(duv)) &
            15),
          "FastCV crop plane pointers must be 16-byte aligned", VA_STATUS_ERROR_INVALID_PARAMETER);
    // FastRPC marshals full stride * height spans, including row padding.
    check(size_t(sy - src.map()) + size_t(src.stride) * r.height <= src.size &&
              size_t(suv - src.map()) + size_t(src.stride) * (r.height / 2) <= src.size &&
              size_t(duv - dst.map()) + size_t(dst.stride) * (target->height / 2) <= dst.size,
          "FastCV plane span exceeds DMA-BUF", VA_STATUS_ERROR_INVALID_SURFACE);
    register_memory(src, runtime_);
    register_memory(dst, runtime_);
    bool down = target->width <= r.width && target->height <= r.height;
    const auto &api = *std::static_pointer_cast<Runtime>(runtime_)->api;
    auto y_scale = down ? api.fcvScaleDownMNu8_fn : api.fcvScaleUpPolyu8_fn;
    auto uv_scale = down ? api.fcvScaleDownMNInterleaveu8_fn : api.fcvScaleUpPolyInterleaveu8_fn;
    y_scale(sy, r.width, r.height, src.stride, dy, target->width, target->height, dst.stride);
    uv_scale(suv, r.width / 2, r.height / 2, src.stride, duv, target->width / 2, target->height / 2,
             dst.stride);
    // FastRPC calls are synchronous; no CPU pixel access or staging allocation.
    target->status = VA_STATUS_SUCCESS;
    trace("VPP FastCV %ux%u -> %ux%u src-fd=%d dst-fd=%d", r.width, r.height, target->width,
          target->height, src.fd, dst.fd);
}
#else
Vpp::Vpp() {
    throw Error(VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT, "FastCV was disabled at build time");
}
void Vpp::scale(const VppPicture &, const std::shared_ptr<Surface> &) {
    throw Error(VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT, "FastCV was disabled at build time");
}
#endif
} // namespace irisva
