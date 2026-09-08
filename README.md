# libva-v4l2

A VA-API driver backed by the Linux V4L2 stateful memory-to-memory decoder
interface. It translates VA-API decode requests into compressed bitstreams for
the Qualcomm Iris driver and exposes decoded frames as VA surfaces and DMA-BUFs.

The current implementation targets **Qualcomm SC8280XP**, tested on the
**Radxa Dragon Q8B** with Iris HFI gen2. It is experimental and is not a generic
backend for every V4L2 M2M device. The driver does not provide software decoding.

## Supported formats

| Codec | VA-API profiles | Bit depth | Surface format |
| --- | --- | --- | --- |
| H.264 / AVC | Constrained Baseline, Main, High | 8-bit | NV12 |
| H.265 / HEVC | Main | 8-bit | NV12 |
| H.265 / HEVC | Main10 | 10-bit | P010 |
| VP9 | Profile 0 | 8-bit | NV12 |
| VP9 | Profile 2 | 10-bit | P010 |

All supported formats are progressive 4:2:0. Encoding and video processing
(VPP) are not implemented.

Basic hardware decoding has been exercised with FFmpeg, GStreamer, mpv, VLC,
Chromium, and Kodi on the target board. Coverage includes pixel comparisons
against software decoding, DMA-BUF import into EGL/GLES, and cached surface
reuse. Application compatibility depends on the codec, rendering path, and
application version; exhaustive stream coverage and long-duration playback
validation remain ongoing.

## Requirements

- Linux with the Qualcomm Iris stateful decoder and MSM DRM drivers.
- The accompanying Iris HFI gen2 kernel patches:
  - [Decode-order output](patches/0001-media-iris-support-decode-order-output-on-HFI-gen2.patch),
    required by the VA-API decode path.
  - [CAPTURE buffer pool limit](patches/0002-media-iris-allow-larger-capture-buffer-pools-on-HFI-.patch),
    required for larger application surface pools, including VLC's default pool.
- A C++17 compiler, Meson 0.61 or newer, Ninja, and pkg-config.
- Development files for libva, libdrm, EGL, GLES 3.2, and GBM.
- Access to the Iris video device and DRM render node.

The backend requires these kernel changes unless equivalent support is already
present. The decode-order patch enables its behavior only when requested by the
backend. Kernel patches must match the kernel being built.
The two patches are provided as references for integrating these changes into
your kernel; this repository does not build or distribute a kernel module package.

## Build and install

On Arch Linux, install the build dependencies:

```sh
sudo pacman -S --needed base-devel meson ninja pkgconf libva libdrm mesa libglvnd
```

Build the userspace driver:

```sh
meson setup build --buildtype=debugoptimized --prefix=/usr
meson compile -C build
```

To use the build without installing it:

```sh
export LIBVA_DRIVER_NAME=v4l2
export LIBVA_DRIVERS_PATH="$PWD/build"
vainfo --display drm --device /dev/dri/renderD128
```

`vainfo` is provided by `libva-utils` on Arch Linux. The expected driver library
is `v4l2_drv_video.so`.

For a system installation, prefer the Arch package below, or run:

```sh
sudo meson install -C build
unset LIBVA_DRIVERS_PATH LIBVA_DRIVER_NAME
```

Installation includes a relative `msm_drv_video.so -> v4l2_drv_video.so`
symlink in the driver directory. This lets libva discover the backend
automatically on Qualcomm MSM devices, including the tested DRM, Wayland, and
X11 paths. `LIBVA_DRIVER_NAME=v4l2` remains available as an explicit override
and is needed for the uninstalled build example above.

Ensure Meson's `libdir` matches libva's driver search path. Set
`-Dlibdir=lib` during configuration on systems using `/usr/lib/dri`.

## Usage

With the driver installed on the target MSM platform, no driver-selection
environment variable is required. For example:

```sh
mpv --hwdec=vaapi input.mp4
```

To decode with FFmpeg and download 8-bit frames for checksum validation:

```sh
ffmpeg \
  -threads 1 -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi -i input.mp4 -an \
  -vf hwdownload,format=nv12,format=yuv420p -f framemd5 -
```

For 10-bit HEVC or VP9, use
`-vf hwdownload,format=p010le,format=yuv420p10le` instead.
`hwdownload` copies frames to CPU memory and is intended here for validation.

## Frame sharing and copies

The driver supports read-only DRM PRIME2 exports of NV12 and P010 surfaces.
There are two output paths:

- **Direct export:** a surface exported after decoding shares its V4L2 CAPTURE
  DMA-BUF with the application. A compatible GPU consumer can use this without
  a decoded-frame copy in the backend.
- **Persistent export:** an application that exports a surface before decoding
  may cache its DMA-BUF, as Chromium does. The backend keeps stable storage and
  updates it with a synchronized GPU copy. This is the default compatibility
  path and is not zero-copy.

Zero-copy across the entire playback pipeline also depends on the application's
renderer and display stack. Compressed input is reconstructed and copied even
when decoded frames use direct export.

## Configuration

Set these variables before launching the application; restart existing
processes for changes to take effect.

| Variable | Default | Description |
| --- | --- | --- |
| `LIBVA_DRIVER_NAME` | Automatic discovery | Set to `v4l2` to explicitly select this driver, for example with an uninstalled build. |
| `LIBVA_DRIVERS_PATH` | libva's system path | Override the driver directory, for example with a local build. |
| `IRIS_VAAPI_DEVICE` | Automatic discovery | Select a V4L2 decoder node, such as `/dev/video0`. |
| `IRIS_VAAPI_COPY` | `gpu` | Select `gpu` or `cpu` for the persistent-export copy path. |
| `IRIS_VAAPI_DEBUG` | Unset | Enable backend diagnostics when present, including when set to `0`. |
| `IRIS_VAAPI_DUMP` | Unset | Write the reconstructed compressed bitstream to this file; use for a single decode session. The file is overwritten. |

GPU initialization or copy failures return a VA error; the driver does not
silently switch to CPU copying. To select CPU copying explicitly:

```sh
IRIS_VAAPI_COPY=cpu chromium
```

## Known limitations

- No interlaced decoding, external DMA-BUF import, fragmented frame submission,
  or dynamic resolution changes within a decode context.
- H.264 FMO and SP/SI switching slices are unsupported.
- HEVC RExt/SCC, multilayer streams, and non-4:2:0 formats are unsupported.
  Main10 requires actual 10-bit content and P010 surfaces.
- A new HEVC context must start at a BLA, IDR, or CRA random-access picture.
  Earlier dependent pictures and unavailable RASL leading pictures return decode
  errors immediately. Applications must discard them and continue to the next
  usable random-access point; missing reference pictures cannot be recovered.
- VP9 requires an initial key frame. Profiles 1/3, 12-bit content, and hidden
  frames that refresh no reference slots are unsupported. Some large-resolution
  test streams still fail.
- VPP capability probes can report an unsupported profile even when ordinary
  decoding works.

## Arch Linux package

[packaging/arch/PKGBUILD](packaging/arch/PKGBUILD) builds the userspace driver
from a local source snapshot:

```sh
python3 packaging/make-dist.py
cd packaging/arch
makepkg -si
```

After changing packaged sources, regenerate the archive, update `sha256sums`
with the output of `makepkg -g`, and refresh `.SRCINFO` with
`makepkg --printsrcinfo > .SRCINFO`. Increment `pkgrel` before distributing an
updated package. Source archives exclude local notes, tools, and test results.

## Development

C and C++ sources use the checked-in `.clang-format` configuration. Use
**clang-format 22** for consistent results:

```sh
clang-format -i src/*.c src/*.cpp src/*.hpp
clang-format --dry-run --Werror src/*.c src/*.cpp src/*.hpp
```

When clang-format is available at Meson configuration time, these targets are
also available:

```sh
meson compile -C build format
meson compile -C build format-check
```

Use `meson setup build -Dwerror=true` for a build that treats compiler warnings
as errors. Hardware behavior must be validated on the target platform.

The maintained tree contains the backend in `src/`, kernel patches in
`patches/`, Arch recipes and snapshot tooling in `packaging/`, and VAAPI FITS
capability declarations in `tests/`. Local research, diagnostic tools, and
historical results live under the ignored `tmp/` directory and are not required
to build or package the driver.

## License

The userspace backend is licensed under the [MIT License](LICENSE). The kernel
patches follow the Linux files' GPL-2.0-only license. External test frameworks and
media retain their respective licenses.
