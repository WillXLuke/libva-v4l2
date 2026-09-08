# Hardware regression tests

Run on the supported Iris board. These are hardware integration tests, not host
unit tests. C/C++ files include build commands in their headers; FFmpeg tests
also require `ffprobe` and software H.264 decoding.

| Test | Coverage |
| --- | --- |
| `encoder/ffmpeg-smoke.py` | Profiles, rate control, dimensions, frame counts, keyframes, and decoded quality. |
| `encoder/api-smoke.cpp` | Uploads, direct/staged input, rejected requests, coded buffers, and resource cleanup. |
| `encoder/async.cpp` | Queued versus serial output, QP/IDR changes, synchronization, timeouts, and pending resource destruction. |
| `encoder/writable-export.cpp` | GPU-written input, stable DMA-BUF exports, and GPU fence synchronization. |

To select an uninstalled driver before running a test:

```sh
export LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$PWD/build"
```

Place generated streams and logs under `tmp/`. The async and writable-export
programs take an existing output directory as their first argument.

Optional failure injection helpers (build commands are in their headers):

- `reject-coherent.c`: load with `LD_PRELOAD` to test the CPU/MMAP fallback on
  hardware that normally supports coherent GEM. Works with all encoder tests.
- `delay-completion.c`: load with `LD_PRELOAD` and set
  `IRIS_TEST_DELAY_COMPLETION=1` when running `async` to test caller timeouts and
  the hardware wait watchdog.

For staged async input, set `IRIS_TEST_STAGING=1`; add `IRIS_VAAPI_COPY=cpu` to
exercise CPU staging. Without that override, staging uses the GPU.
