// SPDX-License-Identifier: MIT
// GPU producer regression matching Sunshine: writable PRIME2 export, R8/GR88
// render targets, glFlush (no glFinish), then H.264 VAAPI encoding.
// c++ -std=c++17 writable-export.cpp -o writable-export $(pkg-config --cflags --libs \
//   libavcodec libavutil libva libdrm gbm egl glesv2)
// Usage: writable-export OUTPUT_DIRECTORY [WIDTH HEIGHT FRAMES]
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/opt.h>
}
#include <va/va_drmcommon.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static void require(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
static void avcheck(int result) {
    if (result < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(result, message, sizeof(message));
        require(false, message);
    }
}
static void vacheck(VAStatus status) {
    require(status == VA_STATUS_SUCCESS, vaErrorStr(status));
}
static unsigned sample(unsigned frame, unsigned stripe, unsigned plane, unsigned channel) {
    return plane ? 80 + (frame * 3 + channel * 31) % 96 : 16 + (frame * 7 + stripe * 19) % 220;
}
struct Gpu {
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    gbm_device *gbm = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_image = nullptr;
    Gpu() {
        require(fd >= 0, "open DRM");
        gbm = gbm_create_device(fd);
        require(gbm, "create GBM");
        auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        create_image =
            reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroy_image =
            reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        bind_image = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        require(platform && create_image && destroy_image && bind_image, "EGL entrypoints");
        display = platform(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
        require(eglInitialize(display, nullptr, nullptr), "initialize EGL");
        require(eglBindAPI(EGL_OPENGL_ES_API), "bind GLES");
        const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
        require(context != EGL_NO_CONTEXT &&
                    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context),
                "EGL context");
    }
    ~Gpu() {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglTerminate(display);
        gbm_device_destroy(gbm);
        close(fd);
    }
    void fill(VADisplay va, VASurfaceID surface, unsigned frame) {
        VADRMPRIMESurfaceDescriptor desc{}, again{};
        vacheck(vaExportSurfaceHandle(
            va, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc));
        vacheck(vaExportSurfaceHandle(
            va, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &again));
        require(desc.num_objects == 1 && desc.num_layers == 2 && again.num_objects == 1,
                "NV12 descriptor");
        struct stat first{}, second{};
        require(!fstat(desc.objects[0].fd, &first) && !fstat(again.objects[0].fd, &second) &&
                    first.st_dev == second.st_dev && first.st_ino == second.st_ino,
                "export changed storage");
        close(again.objects[0].fd);
        for (unsigned plane = 0; plane < 2; ++plane) {
            const auto &layer = desc.layers[plane];
            const auto &object = desc.objects[layer.object_index[0]];
            unsigned w = desc.width / (plane + 1), h = desc.height / (plane + 1);
            const EGLint attrs[] = {EGL_WIDTH,
                                    EGLint(w),
                                    EGL_HEIGHT,
                                    EGLint(h),
                                    EGL_LINUX_DRM_FOURCC_EXT,
                                    EGLint(layer.drm_format),
                                    EGL_DMA_BUF_PLANE0_FD_EXT,
                                    object.fd,
                                    EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                                    EGLint(layer.offset[0]),
                                    EGL_DMA_BUF_PLANE0_PITCH_EXT,
                                    EGLint(layer.pitch[0]),
                                    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                                    EGLint(object.drm_format_modifier),
                                    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                                    EGLint(object.drm_format_modifier >> 32),
                                    EGL_NONE};
            auto image =
                create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
            require(image != EGL_NO_IMAGE_KHR, "import writable plane");
            GLuint texture, fbo;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            bind_image(GL_TEXTURE_2D, image);
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                    "plane is not renderable");
            glEnable(GL_SCISSOR_TEST);
            for (unsigned stripe = 0; stripe < 8; ++stripe) {
                unsigned x = stripe * w / 8, end = (stripe + 1) * w / 8;
                glScissor(x, 0, end - x, h);
                glClearColor(sample(frame, stripe, plane, 0) / 255.f,
                             sample(frame, stripe, plane, 1) / 255.f, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            glDisable(GL_SCISSOR_TEST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &texture);
            require(destroy_image(display, image), "destroy EGL image");
        }
        glFlush(); // The backend must wait for this GPU writer before V4L2 QBUF.
        require(glGetError() == GL_NO_ERROR, "GPU write failed");
        close(desc.objects[0].fd);
    }
};
static std::vector<unsigned char> encode(AVBufferRef *device, Gpu *gpu, unsigned w, unsigned h,
                                         unsigned count) {
    AVBufferRef *frames = av_hwframe_ctx_alloc(device);
    require(frames, "allocate frames context");
    auto *hw = reinterpret_cast<AVHWFramesContext *>(frames->data);
    hw->format = AV_PIX_FMT_VAAPI;
    hw->sw_format = AV_PIX_FMT_NV12;
    hw->width = w;
    hw->height = h;
    hw->initial_pool_size = 8;
    avcheck(av_hwframe_ctx_init(frames));
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    require(codec, "h264_vaapi encoder");
    auto *ctx = avcodec_alloc_context3(codec);
    require(ctx, "allocate encoder");
    ctx->width = w;
    ctx->height = h;
    ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    ctx->time_base = AVRational{1, 60};
    ctx->framerate = AVRational{60, 1};
    ctx->gop_size = 30;
    ctx->max_b_frames = 0;
    ctx->refs = 1;
    ctx->profile = AV_PROFILE_H264_HIGH;
    ctx->bit_rate = 20000000;
    ctx->hw_frames_ctx = av_buffer_ref(frames);
    avcheck(av_opt_set(ctx->priv_data, "rc_mode", "CBR", 0));
    avcheck(av_opt_set_int(ctx->priv_data, "async_depth", 4, 0));
    avcheck(avcodec_open2(ctx, codec, nullptr));
    auto *va_device = reinterpret_cast<AVVAAPIDeviceContext *>(
        reinterpret_cast<AVHWDeviceContext *>(device->data)->hwctx);
    auto *packet = av_packet_alloc();
    require(packet, "allocate packet");
    std::vector<unsigned char> bytes;
    unsigned packets = 0;
    auto receive = [&] {
        int result;
        while ((result = avcodec_receive_packet(ctx, packet)) >= 0) {
            bytes.insert(bytes.end(), packet->data, packet->data + packet->size);
            ++packets;
            av_packet_unref(packet);
        }
        require(result == AVERROR(EAGAIN) || result == AVERROR_EOF, "receive packet");
    };
    for (unsigned i = 0; i < count; ++i) {
        auto *frame = av_frame_alloc();
        require(frame, "allocate frame");
        avcheck(av_hwframe_get_buffer(frames, frame, 0));
        if (gpu) {
            gpu->fill(va_device->display, VASurfaceID(uintptr_t(frame->data[3])), i);
        } else {
            auto *cpu = av_frame_alloc();
            require(cpu, "allocate CPU frame");
            cpu->format = AV_PIX_FMT_NV12;
            cpu->width = w;
            cpu->height = h;
            avcheck(av_frame_get_buffer(cpu, 32));
            for (unsigned plane = 0; plane < 2; ++plane)
                for (unsigned y = 0; y < h / (plane + 1); ++y)
                    for (unsigned x = 0; x < w; ++x)
                        cpu->data[plane][y * cpu->linesize[plane] + x] =
                            sample(i, x * 8 / w, plane, plane ? x % 2 : 0);
            avcheck(av_hwframe_transfer_data(frame, cpu, 0));
            av_frame_free(&cpu);
        }
        frame->pts = i;
        avcheck(avcodec_send_frame(ctx, frame));
        av_frame_free(&frame);
        receive();
    }
    avcheck(avcodec_send_frame(ctx, nullptr));
    receive();
    require(packets == count, "missing output frames");
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    av_buffer_unref(&frames);
    return bytes;
}
int main(int argc, char **argv) {
    if (argc != 2 && argc != 5)
        return 2;
    unsigned w = argc == 5 ? std::atoi(argv[2]) : 640;
    unsigned h = argc == 5 ? std::atoi(argv[3]) : 480;
    unsigned frames = argc == 5 ? std::atoi(argv[4]) : 96;
    require(w && !(w % 16) && h && !(h % 2) && frames, "invalid dimensions/count");
    AVBufferRef *device = nullptr;
    avcheck(
        av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", nullptr, 0));
    Gpu gpu;
    auto cpu = encode(device, nullptr, w, h, frames);
    auto rendered = encode(device, &gpu, w, h, frames);
    require(cpu == rendered, "GPU-written and CPU-uploaded bitstreams differ");
    auto *file = std::fopen((std::string(argv[1]) + "/writable.h264").c_str(), "wb");
    require(file && std::fwrite(rendered.data(), 1, rendered.size(), file) == rendered.size(),
            "write");
    require(!std::fclose(file), "close output");
    av_buffer_unref(&device);
    std::printf("PASS: %u GPU-written frames match CPU upload byte-for-byte; writable exports, "
                "stable storage, EGL plane rendering, glFlush fences and async encode\n",
                frames);
}
