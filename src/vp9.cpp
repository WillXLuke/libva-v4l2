// SPDX-License-Identifier: MIT
#include "internal.hpp"

namespace irisva {
std::vector<uint8_t> vp9_bitstream(VAProfile profile, const Vp9Picture &picture) {
    check(picture.has_params && picture.slices.size() == 1 && picture.pending_slices.empty(),
          "VP9 requires one complete frame per picture", VA_STATUS_ERROR_INVALID_BUFFER);
    const auto &p = picture.params;
    unsigned expected_profile = profile == VAProfileVP9Profile2 ? 2 : 0;
    check(p.profile == expected_profile && p.bit_depth == (expected_profile ? 10 : 8) &&
              p.pic_fields.bits.subsampling_x && p.pic_fields.bits.subsampling_y,
          "VP9 supports Profile 0 8-bit / Profile 2 10-bit 4:2:0",
          VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    const auto &bytes = picture.slices[0].bytes;
    // FFmpeg and GStreamer supply the original complete frame. Partial
    // partition buffers allowed by VA cannot drive a stateful decoder.
    check(bytes.size() >= 3 && p.frame_header_length_in_bytes && p.first_partition_size &&
              size_t(p.frame_header_length_in_bytes) + p.first_partition_size <= bytes.size(),
          "VP9 full uncompressed and compressed headers required", VA_STATUS_ERROR_INVALID_BUFFER);
    size_t pos = 0;
    auto bit = [&]() {
        check(pos < bytes.size() * 8, "truncated VP9 header", VA_STATUS_ERROR_INVALID_BUFFER);
        size_t n = pos++;
        return (bytes[n / 8] >> (7 - n % 8)) & 1u;
    };
    unsigned marker = bit() << 1;
    marker |= bit();
    unsigned raw_profile = bit();
    raw_profile |= bit() << 1;
    check(marker == 2 && raw_profile == expected_profile, "VP9 raw header/profile mismatch",
          VA_STATUS_ERROR_INVALID_BUFFER);
    check(!bit(), "VP9 show-existing must reuse the existing VA surface",
          VA_STATUS_ERROR_UNIMPLEMENTED);
    unsigned frame_type = bit(), show_frame = bit(), error_resilient = bit();
    check(frame_type == p.pic_fields.bits.frame_type &&
              show_frame == p.pic_fields.bits.show_frame &&
              error_resilient == p.pic_fields.bits.error_resilient_mode,
          "VP9 raw header/VA flags mismatch", VA_STATUS_ERROR_INVALID_BUFFER);
    trace("VP9 frame profile=%u show=%u key=%u bytes=%zu", raw_profile, show_frame, !frame_type,
          bytes.size());
    if (show_frame)
        return bytes;

    // Stateful decoders suppress invisible alt-ref output, whereas VA needs
    // a completed surface even for a reference-only picture. Keep the raw
    // frame and all entropy/MV state unchanged, then ask VP9 to display the
    // freshly refreshed reference slot. One superframe produces one output.
    auto bits = [&](unsigned count) {
        unsigned v = 0;
        while (count--)
            v = (v << 1) | bit();
        return v;
    };
    unsigned refresh = 0xff;
    if (frame_type) {
        unsigned intra_only = bit();
        if (!error_resilient)
            bits(2); // reset_frame_context
        if (intra_only) {
            check(bits(24) == 0x498342, "VP9 intra sync code", VA_STATUS_ERROR_INVALID_BUFFER);
            if (raw_profile) {
                check(!bit(), "VP9 12-bit unsupported", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
                check(bits(3) != 7, "VP9 RGB unsupported", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
                bit(); // color_range; Profile 2 has implicit 4:2:0
            }
        }
        refresh = bits(8);
    }
    check(refresh, "VP9 invisible frame without a refreshed reference slot",
          VA_STATUS_ERROR_UNIMPLEMENTED);
    unsigned slot = 0;
    while (!(refresh & (1u << slot)))
        ++slot;
    std::vector<uint8_t> out = bytes;
    out.push_back(uint8_t(0x88 | (raw_profile << 3) | slot));
    unsigned magnitude = 1;
    while (magnitude < 4 && bytes.size() >= (uint64_t(1) << (8 * magnitude)))
        ++magnitude;
    uint8_t marker_index = 0xc0 | ((magnitude - 1) << 3) | 1;
    out.push_back(marker_index);
    for (unsigned n : {unsigned(bytes.size()), 1u})
        for (unsigned i = 0; i < magnitude; ++i)
            out.push_back(uint8_t(n >> (8 * i)));
    out.push_back(marker_index);
    trace("VP9 invisible reference slot=%u superframe bytes=%zu", slot, out.size());
    return out;
}
} // namespace irisva
