// SPDX-License-Identifier: MIT
// Board test: c++ -std=c++17 tests/encoder/api-smoke.cpp -lva -lva-drm -o api-smoke
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static void expect(VAStatus actual, VAStatus wanted = VA_STATUS_SUCCESS) {
    if (actual != wanted) {
        std::fprintf(stderr, "Expected %s, got %s\n", vaErrorStr(wanted), vaErrorStr(actual));
        std::exit(1);
    }
}
static unsigned fd_count() {
    auto dir = opendir("/proc/self/fd");
    if (!dir)
        std::exit(1);
    unsigned count = 0;
    while (readdir(dir))
        ++count;
    closedir(dir);
    return count;
}
int main() {
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return 1;
    VADisplay display = vaGetDisplayDRM(fd);
    int major, minor;
    expect(vaInitialize(display, &major, &minor));
    VAConfigID config;
    VAConfigAttrib unsupported{VAConfigAttribEncPackedHeaders, VA_ENC_PACKED_HEADER_SEQUENCE};
    expect(
        vaCreateConfig(display, VAProfileH264High, VAEntrypointEncSlice, &unsupported, 1, &config),
        VA_STATUS_ERROR_ATTR_NOT_SUPPORTED);
    VAConfigAttrib rc{VAConfigAttribRateControl, VA_RC_CQP};
    expect(vaCreateConfig(display, VAProfileH264High, VAEntrypointEncSlice, &rc, 1, &config));
    unsigned initial_fds = 0;
    std::vector<unsigned char> reference;
    for (unsigned iteration = 0; iteration < 20; ++iteration) {
        VASurfaceID surfaces[3];
        // Alternate direct-compatible storage with a larger pitch/height. The
        // latter must stage the top-left 128x128 region without changing pixels.
        unsigned storage_width = iteration % 2 ? 256 : 128;
        unsigned storage_height = iteration % 2 ? 160 : 128;
        expect(vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, storage_width, storage_height,
                                surfaces, 3, nullptr, 0));
        VAContextID context;
        expect(vaCreateContext(display, config, 128, 128, VA_PROGRESSIVE, surfaces, 3, &context));
        VAImageFormat format{};
        format.fourcc = iteration % 2 ? VA_FOURCC_I420 : VA_FOURCC_NV12;
        format.byte_order = VA_LSB_FIRST;
        format.bits_per_pixel = 12;
        VAImage image;
        expect(vaCreateImage(display, &format, 128, 128, &image));
        void *pointer;
        expect(vaMapBuffer(display, image.buf, &pointer));
        std::memset(pointer, 128, image.data_size);
        for (unsigned y = 0; y < 128; ++y)
            std::memset(static_cast<unsigned char *>(pointer) + image.offsets[0] +
                            y * image.pitches[0],
                        32 + y, 128);
        expect(vaUnmapBuffer(display, image.buf));
        expect(vaPutImage(display, surfaces[0], image.image_id, 0, 0, 128, 128, 0, 0, 128, 128));
        expect(vaDestroyImage(display, image.image_id));
        expect(vaDeriveImage(display, surfaces[0], &image));
        expect(vaMapBuffer(display, image.buf, &pointer));
        for (unsigned y = 0; y < 128; ++y)
            for (unsigned x = 0; x < 128; ++x)
                if (static_cast<unsigned char *>(
                        pointer)[image.offsets[0] + y * image.pitches[0] + x] != 32 + y)
                    return 2;
        expect(vaUnmapBuffer(display, image.buf));
        expect(vaDestroyImage(display, image.image_id));
        VABufferID coded;
        expect(vaCreateBuffer(display, context, VAEncCodedBufferType, 1024 * 1024, 1, nullptr,
                              &coded));
        expect(vaMapBuffer(display, coded, &pointer), VA_STATUS_ERROR_OPERATION_FAILED);
        VAEncSequenceParameterBufferH264 sequence{};
        sequence.level_idc = 31;
        sequence.picture_width_in_mbs = sequence.picture_height_in_mbs = 8;
        sequence.seq_fields.bits.frame_mbs_only_flag = 1;
        sequence.seq_fields.bits.chroma_format_idc = 1;
        sequence.ip_period = 1;
        sequence.intra_period = sequence.intra_idr_period = 30;
        sequence.max_num_ref_frames = 1;
        VAEncPictureParameterBufferH264 picture{};
        picture.CurrPic.picture_id = surfaces[1];
        picture.coded_buf = coded;
        picture.pic_init_qp = 24;
        picture.pic_fields.bits.idr_pic_flag = 1;
        picture.pic_fields.bits.reference_pic_flag = 1;
        picture.pic_fields.bits.entropy_coding_mode_flag = 1;
        VAEncSliceParameterBufferH264 slice{};
        slice.num_macroblocks = 64;
        slice.slice_type = 2;
        slice.macroblock_info = VA_INVALID_ID;
        auto submit = [&](VAStatus result) {
            VABufferID params[3];
            expect(vaCreateBuffer(display, context, VAEncSequenceParameterBufferType,
                                  sizeof(sequence), 1, &sequence, &params[0]));
            expect(vaCreateBuffer(display, context, VAEncPictureParameterBufferType,
                                  sizeof(picture), 1, &picture, &params[1]));
            expect(vaCreateBuffer(display, context, VAEncSliceParameterBufferType, sizeof(slice), 1,
                                  &slice, &params[2]));
            expect(vaBeginPicture(display, context, surfaces[0]));
            expect(vaRenderPicture(display, context, params, 3));
            expect(vaEndPicture(display, context), result);
            for (auto id : params)
                expect(vaDestroyBuffer(display, id));
        };
        slice.disable_deblocking_filter_idc = 3;
        submit(VA_STATUS_ERROR_INVALID_PARAMETER);
        slice.disable_deblocking_filter_idc = 0;
        slice.slice_alpha_c0_offset_div2 = 7;
        submit(VA_STATUS_ERROR_INVALID_PARAMETER);
        slice.slice_alpha_c0_offset_div2 = 0;
        slice.slice_beta_offset_div2 = -7;
        submit(VA_STATUS_ERROR_INVALID_PARAMETER);
        slice.slice_beta_offset_div2 = 0;
        submit(VA_STATUS_SUCCESS);
        VAStatus polled = vaSyncBuffer(display, coded, 0);
        if (polled != VA_STATUS_SUCCESS && polled != VA_STATUS_ERROR_TIMEDOUT)
            expect(polled);
        expect(vaSyncBuffer(display, coded, VA_TIMEOUT_INFINITE));
        expect(vaMapBuffer(display, coded, &pointer));
        auto segment = static_cast<VACodedBufferSegment *>(pointer);
        if (!segment->size || !segment->buf || segment->next || segment->status)
            return 3;
        auto *bytes = static_cast<unsigned char *>(segment->buf);
        if (reference.empty())
            reference.assign(bytes, bytes + segment->size);
        else if (reference.size() != segment->size ||
                 std::memcmp(reference.data(), bytes, segment->size)) {
            std::fprintf(stderr, "Direct/staged encode mismatch at iteration %u\n", iteration);
            return 5;
        }
        // A mapped bitstream cannot be overwritten by another encode submission.
        submit(VA_STATUS_ERROR_SURFACE_BUSY);
        expect(vaUnmapBuffer(display, coded));
        picture.pic_fields.bits.idr_pic_flag = 0;
        slice.slice_type = 1;
        submit(VA_STATUS_ERROR_UNIMPLEMENTED); // B frames are not silently encoded as P.
        slice.slice_type = 0;
        picture.CurrPic.picture_id = surfaces[2];
        slice.RefPicList0[0].picture_id = surfaces[2];
        submit(VA_STATUS_ERROR_UNIMPLEMENTED); // Unsupported reference selection.
        slice.RefPicList0[0].picture_id = surfaces[1];
        slice.slice_alpha_c0_offset_div2 = slice.slice_beta_offset_div2 = 2;
        submit(VA_STATUS_ERROR_UNIMPLEMENTED); // Static controls need a new context.
        slice.slice_alpha_c0_offset_div2 = slice.slice_beta_offset_div2 = 0;
        submit(VA_STATUS_SUCCESS); // Validation errors did not poison the session.
        expect(vaSyncSurface(display, surfaces[0]));
        expect(vaDestroyBuffer(display, coded));
        if (iteration == 0) {
            expect(vaCreateBuffer(display, context, VAEncCodedBufferType, 1, 1, nullptr, &coded));
            picture.coded_buf = coded;
            picture.pic_fields.bits.idr_pic_flag = 1;
            slice.slice_type = 2;
            submit(VA_STATUS_SUCCESS); // Hardware completion is asynchronous.
            expect(vaSyncBuffer(display, coded, VA_TIMEOUT_INFINITE),
                   VA_STATUS_ERROR_NOT_ENOUGH_BUFFER);
            expect(vaSyncSurface(display, surfaces[0]), VA_STATUS_ERROR_NOT_ENOUGH_BUFFER);
            expect(vaDestroyBuffer(display, coded));
        }
        expect(vaDestroyContext(display, context));
        expect(vaDestroySurfaces(display, surfaces, 3));
        unsigned count = fd_count();
        if (iteration == 1)
            initial_fds = count; // Account for lazy EGL/driver initialization.
        if (iteration > 1 && count != initial_fds) {
            std::fprintf(stderr, "FD leak: baseline=%u now=%u\n", initial_fds, count);
            return 4;
        }
    }
    expect(vaDestroyConfig(display, config));
    expect(vaTerminate(display));
    close(fd);
    std::puts(
        "PASS: uploads, deblocking validation, coded mapping/sync, invalid requests, overflow, "
        "20 context "
        "lifecycles, fd counts");
}
