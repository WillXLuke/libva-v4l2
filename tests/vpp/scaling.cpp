// SPDX-License-Identifier: MIT
// Hardware integration test: CPU reference runs in a child before VA/FastCV init.
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#include <drm_fourcc.h>
#include <fastcv/fastcv.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <misc/fastrpc.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>

static int input_fd = -1, output_fd = -1, direct_calls = 0;
static bool same_buffer(int a, int b) {
    struct stat x{}, y{};
    return a >= 0 && b >= 0 && !fstat(a, &x) && !fstat(b, &y) && x.st_dev == y.st_dev &&
           x.st_ino == y.st_ino;
}
extern "C" int ioctl(int fd, unsigned long request, ...) noexcept {
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    static auto real =
        reinterpret_cast<int (*)(int, unsigned long, ...)>(dlsym(RTLD_NEXT, "ioctl"));
    bool both = false;
    if (request == FASTRPC_IOCTL_INVOKE && arg) {
        auto *call = static_cast<fastrpc_invoke *>(arg);
        auto *buffers = reinterpret_cast<fastrpc_invoke_args *>(call->args);
        unsigned n = ((call->sc >> 16) & 255) + ((call->sc >> 8) & 255) + ((call->sc >> 4) & 15) +
                     (call->sc & 15);
        bool in = false, out = false;
        for (unsigned i = 0; i < n; ++i) {
            in |= same_buffer(buffers[i].fd, input_fd);
            out |= same_buffer(buffers[i].fd, output_fd);
        }
        both = in && out;
    }
    int result = real(fd, request, arg);
    if (!result && both)
        ++direct_calls;
    return result;
}
static void require(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
static void va(VAStatus status) {
    if (status) {
        std::fprintf(stderr, "VA: %s\n", vaErrorStr(status));
        std::exit(1);
    }
}
static void sync(int fd, uint64_t flags) {
    dma_buf_sync s{flags};
    require(ioctl(fd, DMA_BUF_IOCTL_SYNC, &s) == 0, "DMA-BUF CPU sync");
}
struct Allocation {
    VADRMPRIMESurfaceDescriptor desc{};
    uint8_t *data;
    Allocation(unsigned w, unsigned h) {
        auto &obj = desc.objects[0];
        auto &l = desc.layers[0];
        desc.fourcc = l.drm_format = DRM_FORMAT_NV12;
        desc.width = w;
        desc.height = h;
        desc.num_objects = desc.num_layers = 1;
        l.num_planes = 2;
        l.pitch[0] = l.pitch[1] = (w + 127) & ~127u;
        l.offset[0] = 128; // Nonzero base offset and padded Y rows are intentional.
        l.offset[1] = 128 + l.pitch[0] * ((h + 31) & ~31u);
        obj.size = (l.offset[1] + l.pitch[1] * ((h + 1) / 2) + 4095) & ~4095u;
        obj.drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
        int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
        require(heap >= 0, "open system heap");
        dma_heap_allocation_data a{};
        a.len = obj.size;
        a.fd_flags = O_RDWR | O_CLOEXEC;
        require(!ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a), "allocate DMA-BUF");
        close(heap);
        obj.fd = a.fd;
        data = static_cast<uint8_t *>(
            mmap(nullptr, obj.size, PROT_READ | PROT_WRITE, MAP_SHARED, obj.fd, 0));
        require(data != MAP_FAILED, "map allocation");
    }
    ~Allocation() {
        munmap(data, desc.objects[0].size);
        close(desc.objects[0].fd);
    }
};
static VASurfaceID import(VADisplay display, VADRMPRIMESurfaceDescriptor &desc) {
    VASurfaceAttrib attrs[2]{};
    attrs[0].type = VASurfaceAttribMemoryType;
    attrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[0].value.type = VAGenericValueTypeInteger;
    attrs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attrs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[1].value.type = VAGenericValueTypePointer;
    auto owned = desc;
    owned.objects[0].fd = dup(desc.objects[0].fd);
    require(owned.objects[0].fd >= 0, "duplicate descriptor fd");
    attrs[1].value.value.p = &owned;
    VASurfaceID id;
    va(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, desc.width, desc.height, &id, 1, attrs, 2));
    close(owned.objects[0].fd); // Driver must own its own fd after import.
    return id;
}
struct Case {
    unsigned sw, sh, dw, dh;
    bool crop;
};
static void run_case(Case c) {
    Allocation input(c.sw, c.sh), reference(c.dw, c.dh), destination(c.dw, c.dh);
    auto &sl = input.desc.layers[0];
    sync(input.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    std::memset(input.data, 0xa5, input.desc.objects[0].size);
    for (unsigned p = 0; p < 2; ++p)
        for (unsigned y = 0; y < (p ? c.sh / 2 : c.sh); ++y)
            for (unsigned x = 0; x < c.sw; ++x)
                input.data[sl.offset[p] + y * sl.pitch[p] + x] =
                    p ? (x & 1 ? 150 + (x / 2 + y) % 80 : 20 + (x / 2 + y) % 80)
                      : 16 + (x / 8 + y / 4) % 220;
    sync(input.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    VARectangle region{static_cast<int16_t>(c.crop ? 16 : 0), static_cast<int16_t>(c.crop ? 2 : 0),
                       static_cast<uint16_t>(c.crop ? 608 : c.sw),
                       static_cast<uint16_t>(c.crop ? 360 : c.sh)};
    pid_t child = fork();
    require(child >= 0, "fork reference");
    if (!child) {
        require(!fcvSetOperationMode(FASTCV_OP_CPU_PERFORMANCE), "CPU reference mode");
        fcvMemInit();
        sync(input.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        sync(reference.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        bool down = c.dw <= region.width && c.dh <= region.height;
        auto &rl = reference.desc.layers[0];
        auto yscale = down ? fcvScaleDownMNu8 : fcvScaleUpPolyu8;
        auto uvscale = down ? fcvScaleDownMNInterleaveu8 : fcvScaleUpPolyInterleaveu8;
        yscale(input.data + sl.offset[0] + region.y * sl.pitch[0] + region.x, region.width,
               region.height, sl.pitch[0], reference.data + rl.offset[0], c.dw, c.dh, rl.pitch[0]);
        uvscale(input.data + sl.offset[1] + (region.y / 2) * sl.pitch[1] + region.x,
                region.width / 2, region.height / 2, sl.pitch[1], reference.data + rl.offset[1],
                c.dw / 2, c.dh / 2, rl.pitch[1]);
        sync(reference.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        sync(input.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        fcvMemDeInit();
        fcvCleanUp();
        _exit(0);
    }
    int status;
    require(waitpid(child, &status, 0) == child && status == 0, "CPU reference child");
    int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    require(drm >= 0, "open DRM");
    VADisplay display = vaGetDisplayDRM(drm);
    int major, minor;
    va(vaInitialize(display, &major, &minor));
    VAConfigID config;
    VAContextID context;
    va(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, nullptr, 0, &config));
    va(vaCreateContext(display, config, 0, 0, 0, nullptr, 0, &context));
    unsigned n = 0;
    va(vaQueryVideoProcFilters(display, context, nullptr, &n));
    require(n == 0, "scaling has no filters");
    VAProcPipelineCaps caps{};
    va(vaQueryVideoProcPipelineCaps(display, context, nullptr, 0, &caps));
    require(caps.num_input_pixel_formats == 1, "NV12 capability count");
    require(caps.input_pixel_format && caps.input_pixel_format[0] == VA_FOURCC_NV12,
            "NV12 capability value");
    auto in = import(display, input.desc);
    // Test composed and separate-layer PRIME2 imports.
    auto separate = destination.desc;
    separate.num_layers = 2;
    separate.layers[0].drm_format = DRM_FORMAT_R8;
    separate.layers[0].num_planes = 1;
    separate.layers[1].drm_format = DRM_FORMAT_GR88;
    separate.layers[1].num_planes = 1;
    separate.layers[1].pitch[0] = destination.desc.layers[0].pitch[1];
    separate.layers[1].offset[0] = destination.desc.layers[0].offset[1];
    VASurfaceID outputs[3];
    outputs[0] = import(display, separate);
    va(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, c.dw, c.dh, &outputs[1], 2, nullptr, 0));
    for (unsigned kind = 0; kind < 3; ++kind) {
        VADRMPRIMESurfaceDescriptor exported{};
        // kind 0: external heap; kind 1: pre-exported MSM; kind 2: lazy VPP heap.
        if (kind != 2) {
            va(vaExportSurfaceHandle(
                display, outputs[kind], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &exported));
            output_fd = exported.objects[0].fd;
        } else
            output_fd = -1;
        input_fd = input.desc.objects[0].fd;
        direct_calls = 0;
        for (int repeat = 0; repeat < 3; ++repeat) {
            VAProcPipelineParameterBuffer params{};
            params.surface = in;
            params.surface_region = &region;
            VABufferID buffer;
            va(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(params),
                              1, &params, &buffer));
            va(vaBeginPicture(display, context, outputs[kind]));
            va(vaRenderPicture(display, context, &buffer, 1));
            va(vaDestroyBuffer(display, buffer)); // Parameters must already be snapshotted.
            va(vaEndPicture(display, context));
            va(vaSyncSurface(display, outputs[kind]));
            if (kind == 2 && repeat == 0) {
                va(vaExportSurfaceHandle(
                    display, outputs[kind], VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                    VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &exported));
                output_fd = exported.objects[0].fd;
            }
        }
        require(direct_calls == (kind == 2 ? 4 : 6),
                "both original DMA-BUFs must reach successful FastRPC calls");
        VAImage image{};
        va(vaDeriveImage(display, outputs[kind], &image));
        void *mapped;
        va(vaMapBuffer(display, image.buf, &mapped));
        sync(reference.desc.objects[0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        unsigned maxdiff[2]{};
        for (unsigned p = 0; p < 2; ++p)
            for (unsigned y = 0; y < (p ? c.dh / 2 : c.dh); ++y)
                for (unsigned x = 0; x < c.dw; ++x) {
                    int a =
                        static_cast<uint8_t *>(mapped)[image.offsets[p] + y * image.pitches[p] + x];
                    int b = reference.data[reference.desc.layers[0].offset[p] +
                                           y * reference.desc.layers[0].pitch[p] + x];
                    maxdiff[p] = std::max(maxdiff[p], unsigned(std::abs(a - b)));
                }
        sync(reference.desc.objects[0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        va(vaUnmapBuffer(display, image.buf));
        va(vaDestroyImage(display, image.image_id));
        std::printf("%ux%u -> %ux%u crop=%d output=%u RPC=%d maxdiff Y=%u UV=%u\n", c.sw, c.sh,
                    c.dw, c.dh, c.crop, kind, direct_calls, maxdiff[0], maxdiff[1]);
        require(maxdiff[0] <= 1 && maxdiff[1] <= 1,
                "DSP matches CPU reference within rounding tolerance");
        close(exported.objects[0].fd);
    }
    // Unsupported operations must fail before a DSP invocation.
    VAProcPipelineParameterBuffer bad{};
    bad.surface = in;
    bad.rotation_state = VA_ROTATION_90;
    VABufferID buffer;
    va(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(bad), 1, &bad,
                      &buffer));
    va(vaBeginPicture(display, context, outputs[0]));
    require(vaRenderPicture(display, context, &buffer, 1) == VA_STATUS_ERROR_UNIMPLEMENTED,
            "reject rotation");
    require(vaEndPicture(display, context) == VA_STATUS_ERROR_INVALID_PARAMETER,
            "reject missing parameters");
    va(vaDestroyBuffer(display, buffer));
    auto alias = import(display, input.desc);
    bad = {};
    bad.surface = in;
    va(vaCreateBuffer(display, context, VAProcPipelineParameterBufferType, sizeof(bad), 1, &bad,
                      &buffer));
    va(vaBeginPicture(display, context, alias));
    va(vaRenderPicture(display, context, &buffer, 1));
    int before = direct_calls;
    require(vaEndPicture(display, context) == VA_STATUS_ERROR_INVALID_SURFACE,
            "reject DMA-BUF alias");
    require(direct_calls == before, "alias must not reach DSP");
    va(vaDestroyBuffer(display, buffer));
    va(vaDestroySurfaces(display, &alias, 1));
    va(vaDestroyContext(display, context));
    va(vaDestroySurfaces(display, &in, 1));
    va(vaDestroySurfaces(display, outputs, 3));
    va(vaDestroyConfig(display, config));
    va(vaTerminate(display));
    close(drm);
}
int main() {
    const Case cases[] = {{3840, 2160, 1920, 1080, false}, {640, 360, 1280, 720, false},
                          {640, 360, 640, 360, false},     {640, 384, 320, 180, true},
                          {16, 2, 32, 4, false},           {32, 4, 16, 2, false}};
    // FastCV/FastRPC own threads and process-global state. Never fork a process
    // that has already initialized them: each case starts in a fresh child.
    for (auto c : cases) {
        pid_t child = fork();
        require(child >= 0, "fork test case");
        if (!child) {
            run_case(c);
            std::exit(0);
        }
        int status;
        require(waitpid(child, &status, 0) == child && status == 0, "test case child");
    }
    std::puts("PASS");
}
