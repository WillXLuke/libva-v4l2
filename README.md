# libva-v4l2

A VA-API backend for Qualcomm Iris, using the Linux V4L2 stateful M2M interface.
It provides hardware video decoding and H.264 encoding on **SC8280XP**, tested on
**Radxa Dragon Q8B**. The project is experimental and targets this platform;
it is not a generic backend for all V4L2 devices.

## Support

| Codec | Decoding | Encoding |
| --- | --- | --- |
| H.264 / AVC | Constrained Baseline, Main, High (8-bit) | Same profiles (8-bit) |
| H.265 / HEVC | Main, Main10 (8/10-bit) | — |
| VP9 | Profiles 0 and 2 (8/10-bit) | — |

Supported video is progressive 4:2:0. Decoding has been tested with FFmpeg,
GStreamer, mpv, VLC, Chromium, and Kodi; H.264 encoding with FFmpeg, GStreamer,
and Sunshine.

- H.264 encoding supports CQP, CBR, and VBR, even dimensions from 128×128 to
  3840×2160, and IDR/P frames with one reference. B frames are unsupported.
- Video processing (VPP), hardware scaling, and bit-depth conversion are not
  implemented. Some streams and advanced codec features remain unsupported.
- Compatible DMA-BUF paths avoid raw-frame copies. Applications that cache
  surfaces before decoding use a GPU copy by default.

## Requirements

- Linux with the Qualcomm Iris stateful driver and MSM DRM support.
- The changes supplied in [patches/](patches/), unless already present in your
  kernel: decode-order output, larger CAPTURE pools, and encoder deblocking
  controls. The patches are references to integrate into your kernel.
- Access to the Iris video devices and DRM render node.
- A C++17 compiler, Meson ≥ 0.61, Ninja, pkg-config, and development libraries
  for libva, libdrm, EGL, GLES 3.2, and GBM.

## Build and install

On Arch Linux:

```sh
sudo pacman -S --needed base-devel meson ninja pkgconf libva libdrm mesa libglvnd
python3 packaging/make-dist.py
cd packaging/arch
makepkg -si
```

The [PKGBUILD](packaging/arch/PKGBUILD) packages the userspace driver. After
editing packaged sources, regenerate the archive, update `sha256sums` using
`makepkg -g`, and refresh `.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO`.

Alternatively, build and install with Meson:

```sh
meson setup build --buildtype=release --prefix=/usr -Dlibdir=lib
meson compile -C build
sudo meson install -C build
```

Adjust `libdir` if your system uses a driver directory other than `/usr/lib/dri`.
Installation adds the MSM driver alias, so **`LIBVA_DRIVER_NAME` is not required**
on the target platform.

To check the installation (`vainfo` comes from `libva-utils` on Arch):

```sh
vainfo --display drm --device /dev/dri/renderD128
```

## Usage

Play a video with mpv:

```sh
mpv --hwdec=vaapi input.mp4
```

Encode to H.264 with FFmpeg:

```sh
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 -an \
  -vf format=nv12,hwupload -c:v h264_vaapi -profile:v high \
  -bf 0 -g 60 -rc_mode CBR -b:v 6M -async_depth 4 output.mp4
```

For hardware decoding and encoding of a supported 8-bit input:

```sh
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi -i input.mp4 -an \
  -c:v h264_vaapi -bf 0 -qp 24 -async_depth 4 output.mp4
```

MP4 and raw H.264 output are supported; for Matroska, encode to raw H.264 first
and remux. FFmpeg may warn about unsupported packed headers because the firmware
produces the headers. Custom VUI/SEI metadata is not forwarded.

For GStreamer, set `GST_VA_ALL_DRIVERS=1` to enable this driver's `va` elements:

```sh
GST_VA_ALL_DRIVERS=1 gst-launch-1.0 -e \
  videotestsrc num-buffers=120 ! \
  video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! \
  vah264enc rate-control=vbr bitrate=6000 target-percentage=100 \
    b-frames=0 ref-frames=1 target-usage=1 key-int-max=60 ! \
  'video/x-h264,profile=high' ! h264parse ! mp4mux ! filesink location=output.mp4
```

For Sunshine/Moonlight, use **H.264 / SDR**. HEVC and AV1 encoder probes may fail
while H.264 remains available.

## Configuration and development

| Variable | Purpose |
| --- | --- |
| `LIBVA_DRIVER_NAME=v4l2` | Explicitly select this backend. |
| `LIBVA_DRIVERS_PATH=/path/to/build` | Load an uninstalled build. |
| `IRIS_VAAPI_DEVICE` / `IRIS_VAAPI_ENCODER_DEVICE` | Override automatic video-device discovery. |
| `IRIS_VAAPI_COPY=gpu` or `cpu` | Select the copy path; default is `gpu`. |
| `IRIS_VAAPI_DEBUG=1` | Enable diagnostics. |

Use clang-format 22 and the checked-in `.clang-format`. Meson provides `format`
and `format-check` targets when clang-format is available; `-Dwerror=true`
enables compiler warnings as errors. Hardware tests and their run instructions
are in [tests/](tests/). Local research and historical results live in ignored
`tmp/` and are not required to build the driver.

## License

The backend is [MIT licensed](LICENSE). Bundled Linux kernel patches are
GPL-2.0-only; external test frameworks and media retain their own licenses.
