// SPDX-License-Identifier: MIT
// Board test: c++ -std=c++17 async.cpp -lva -lva-drm -o async
// Usage: async OUTPUT_DIRECTORY (must already exist)
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

static void expect(VAStatus got, VAStatus wanted = VA_STATUS_SUCCESS) {
    if (got != wanted) {
        std::fprintf(stderr, "Expected %s, got %s\n", vaErrorStr(wanted), vaErrorStr(got));
        std::exit(1);
    }
}
static void require(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
static unsigned fds() {
    auto *dir = opendir("/proc/self/fd");
    require(dir, "opendir");
    unsigned n = 0;
    while (readdir(dir))
        ++n;
    closedir(dir);
    return n;
}
struct Session {
    static constexpr unsigned count = 12, width = 640, height = 480;
    VADisplay display;
    VAConfigID config;
    VAContextID context;
    VASurfaceID inputs[count], recon[count];
    VABufferID coded[count];
    explicit Session(VADisplay d, bool overflow = false) : display(d) {
        VAConfigAttrib attr{VAConfigAttribRateControl, VA_RC_CQP};
        expect(vaCreateConfig(d, VAProfileH264High, VAEntrypointEncSlice, &attr, 1, &config));
        bool staging = std::getenv("IRIS_TEST_STAGING") != nullptr;
        expect(vaCreateSurfaces(d, VA_RT_FORMAT_YUV420, staging ? 768 : width,
                                staging ? 512 : height, inputs, count, nullptr, 0));
        expect(vaCreateSurfaces(d, VA_RT_FORMAT_YUV420, width, height, recon, count, nullptr, 0));
        expect(vaCreateContext(d, config, width, height, VA_PROGRESSIVE, inputs, count, &context));
        VAImageFormat format{};
        format.fourcc = VA_FOURCC_NV12;
        format.byte_order = VA_LSB_FIRST;
        format.bits_per_pixel = 12;
        VAImage image;
        expect(vaCreateImage(d, &format, width, height, &image));
        for (unsigned i = 0; i < count; ++i) {
            void *data;
            expect(vaMapBuffer(d, image.buf, &data));
            std::memset(data, 128, image.data_size);
            for (unsigned y = 0; y < height; ++y)
                for (unsigned x = 0; x < width; ++x)
                    static_cast<unsigned char *>(
                        data)[image.offsets[0] + y * image.pitches[0] + x] =
                        16 + ((x / 8 + y / 8 + i * 7) % 200);
            expect(vaUnmapBuffer(d, image.buf));
            expect(
                vaPutImage(d, inputs[i], image.image_id, 0, 0, width, height, 0, 0, width, height));
            expect(vaCreateBuffer(d, context, VAEncCodedBufferType,
                                  overflow && !i ? 1 : width * height * 3, 1, nullptr, &coded[i]));
        }
        expect(vaDestroyImage(d, image.image_id));
    }
    ~Session() {
        if (context != VA_INVALID_ID)
            expect(vaDestroyContext(display, context));
        for (auto id : coded)
            if (id != VA_INVALID_ID)
                expect(vaDestroyBuffer(display, id));
        for (auto id : inputs)
            if (id != VA_INVALID_SURFACE)
                expect(vaDestroySurfaces(display, &id, 1));
        expect(vaDestroySurfaces(display, recon, count));
        expect(vaDestroyConfig(display, config));
    }
    void submit(unsigned frame, VAStatus wanted = VA_STATUS_SUCCESS) {
        unsigned i = frame % count;
        VAEncSequenceParameterBufferH264 seq{};
        seq.level_idc = 31;
        seq.picture_width_in_mbs = width / 16;
        seq.picture_height_in_mbs = height / 16;
        seq.seq_fields.bits.frame_mbs_only_flag = 1;
        seq.seq_fields.bits.chroma_format_idc = 1;
        seq.ip_period = 1;
        seq.intra_period = seq.intra_idr_period = 17;
        seq.max_num_ref_frames = 1;
        VAEncPictureParameterBufferH264 pic{};
        pic.CurrPic.picture_id = recon[i];
        pic.coded_buf = coded[i];
        pic.pic_init_qp = (frame / 23) % 2 ? 28 : 24;
        pic.pic_fields.bits.idr_pic_flag = frame % 17 == 0;
        pic.pic_fields.bits.reference_pic_flag = 1;
        pic.pic_fields.bits.entropy_coding_mode_flag = 1;
        VAEncSliceParameterBufferH264 slice{};
        slice.num_macroblocks = width / 16 * (height / 16);
        slice.slice_type = frame % 17 == 0 ? 2 : 0;
        slice.macroblock_info = VA_INVALID_ID;
        slice.RefPicList0[0].picture_id = recon[(frame + count - 1) % count];
        VABufferID parameters[3];
        expect(vaCreateBuffer(display, context, VAEncSequenceParameterBufferType, sizeof(seq), 1,
                              &seq, &parameters[0]));
        expect(vaCreateBuffer(display, context, VAEncPictureParameterBufferType, sizeof(pic), 1,
                              &pic, &parameters[1]));
        expect(vaCreateBuffer(display, context, VAEncSliceParameterBufferType, sizeof(slice), 1,
                              &slice, &parameters[2]));
        expect(vaBeginPicture(display, context, inputs[i]));
        expect(vaRenderPicture(display, context, parameters, 3));
        expect(vaEndPicture(display, context), wanted);
        for (auto id : parameters)
            expect(vaDestroyBuffer(display, id));
    }
    void collect(unsigned frame, std::vector<unsigned char> &stream) {
        unsigned i = frame % count;
        if (frame % 3 == 0) {
            auto polled = vaSyncBuffer(display, coded[i], 0);
            require(polled == VA_STATUS_SUCCESS || polled == VA_STATUS_ERROR_TIMEDOUT,
                    "nonblocking sync");
            expect(vaSyncBuffer(display, coded[i], VA_TIMEOUT_INFINITE));
        } else if (frame % 3 == 1 && inputs[i] != VA_INVALID_SURFACE) {
            expect(vaSyncSurface(display, inputs[i]));
        }
        // Every third frame deliberately maps without an explicit sync.
        void *data;
        expect(vaMapBuffer(display, coded[i], &data));
        auto *seg = static_cast<VACodedBufferSegment *>(data);
        require(seg->size && seg->buf && !seg->next && !seg->status, "invalid coded segment");
        auto *bytes = static_cast<unsigned char *>(seg->buf);
        stream.insert(stream.end(), bytes, bytes + seg->size);
        expect(vaUnmapBuffer(display, coded[i]));
        VASurfaceStatus status;
        expect(vaQuerySurfaceStatus(display, recon[i], &status));
        require(status == VASurfaceReady, "reconstruction status is not ready after sync");
    }
};
static std::vector<unsigned char> encode(VADisplay d, unsigned depth) {
    Session session(d);
    std::vector<unsigned char> stream;
    constexpr unsigned frames = 96;
    for (unsigned i = 0; i < frames; ++i) {
        if (i >= depth)
            session.collect(i - depth, stream);
        session.submit(i);
    }
    for (unsigned i = frames - depth; i < frames; ++i)
        session.collect(i, stream);
    return stream;
}
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    require(fd >= 0, "DRM open");
    VADisplay display = vaGetDisplayDRM(fd);
    int major, minor;
    expect(vaInitialize(display, &major, &minor));
    auto serial = encode(display, 1);
    auto parallel = encode(display, 8);
    require(serial == parallel, "async changed QP/IDR/reference behavior");
    FILE *file = std::fopen((std::string(argv[1]) + "/async.h264").c_str(), "wb");
    require(file && std::fwrite(parallel.data(), 1, parallel.size(), file) == parallel.size(),
            "write");
    require(!std::fclose(file), "close");
    // Application think time is not an active hardware wait timeout.
    {
        Session s(display);
        for (unsigned i = 0; i < 8; ++i)
            s.submit(i);
        std::this_thread::sleep_for(std::chrono::seconds(6));
        std::vector<unsigned char> output;
        for (unsigned i = 0; i < 8; ++i)
            s.collect(i, output);
        require(!output.empty(), "lost output while client was idle");
    }
    unsigned baseline = fds();
    for (unsigned round = 0; round < 10; ++round) {
        {
            Session s(display);
            for (unsigned i = 0; i < 4; ++i)
                s.submit(i);
            for (unsigned i = 0; i < 4; ++i) {
                expect(vaDestroySurfaces(display, &s.inputs[i], 1));
                s.inputs[i] = VA_INVALID_SURFACE;
            }
            expect(vaDestroyBuffer(display, s.coded[1]));
            s.coded[1] = VA_INVALID_ID;
            // Destruction drains outstanding work, preserving surviving coded buffers.
            expect(vaDestroyContext(display, s.context));
            s.context = VA_INVALID_ID;
            std::vector<unsigned char> output;
            for (unsigned i : {0u, 2u, 3u})
                s.collect(i, output);
            require(!output.empty(), "lost output at context destruction");
        }
        require(fds() == baseline, "FD leak after destroying queued resources");
    }
    {
        Session s(display, true);
        s.submit(0);
        expect(vaSyncBuffer(display, s.coded[0], VA_TIMEOUT_INFINITE),
               VA_STATUS_ERROR_NOT_ENOUGH_BUFFER);
        void *data;
        expect(vaMapBuffer(display, s.coded[0], &data), VA_STATUS_ERROR_NOT_ENOUGH_BUFFER);
        s.submit(1, VA_STATUS_ERROR_ENCODING_ERROR); // A distinct, unused input surface.
    }
    require(fds() == baseline, "FD leak after asynchronous overflow");
    if (std::getenv("IRIS_TEST_DELAY_COMPLETION")) {
        // Requires delay-completion.so. Keep the hardware running while hiding
        // DQBUF completions, then make them visible again after a caller timeout.
        {
            Session s(display);
            setenv("IRIS_TEST_BLOCK_DQBUF", "1", 1);
            s.submit(0);
            expect(vaSyncBuffer(display, s.coded[0], 0), VA_STATUS_ERROR_TIMEDOUT);
            expect(vaSyncBuffer(display, s.coded[0], 1000000), VA_STATUS_ERROR_TIMEDOUT);
            unsetenv("IRIS_TEST_BLOCK_DQBUF");
            std::vector<unsigned char> output;
            s.collect(0, output);
        }
        {
            Session s(display);
            setenv("IRIS_TEST_BLOCK_DQBUF", "1", 1);
            s.submit(0);
            expect(vaSyncBuffer(display, s.coded[0], VA_TIMEOUT_INFINITE),
                   VA_STATUS_ERROR_ENCODING_ERROR);
            unsetenv("IRIS_TEST_BLOCK_DQBUF");
            void *data;
            expect(vaMapBuffer(display, s.coded[0], &data), VA_STATUS_ERROR_ENCODING_ERROR);
        }
        require(fds() == baseline, "FD leak after timeout recovery/abort");
        std::puts(
            "PASS: caller timeouts preserve jobs; active hardware wait timeout aborts safely");
    }
    expect(vaTerminate(display));
    close(fd);
    std::puts("PASS: serial/async bitstreams identical, sync/map/status, QP/IDR barriers, pending "
              "resource destruction, idle client, overflow and FD counts");
}
