project(
  'libva-v4l2', 'cpp', 'c',
  version: '1.0.0',
  meson_version: '>=0.61.0',
  license: 'MIT',
  default_options: ['cpp_std=c++17', 'warning_level=2'],
)

va = dependency('libva', version: '>=1.10')
drm = dependency('libdrm')
threads = dependency('threads')
egl = dependency('egl')
gles = dependency('glesv2', version: '>=3.2')
gbm = dependency('gbm')

cpp = meson.get_compiler('cpp')
fastcv_option = get_option('fastcv')
fastcv = cpp.find_library('fastcvopt', required: fastcv_option)
cdsprpc = cpp.find_library('cdsprpc', required: fastcv_option)
have_fastcv = false
if not fastcv_option.disabled() and fastcv.found() and cdsprpc.found()
  have_fastcv = cpp.links('''
    #include <fastcv/fastcv.h>
    #include <fastcv/fastcvExt.h>
    extern "C" void remote_register_buf(void *, int, int);
    int main() {
      fcvSetOperationModeExt(FASTCV_OP_EXT_QDSP);
      fcvMemInit();
      remote_register_buf(nullptr, 0, -1);
      fcvScaleDownMNu8(nullptr, 0, 0, 0, nullptr, 0, 0, 0);
      fcvScaleDownMNInterleaveu8(nullptr, 0, 0, 0, nullptr, 0, 0, 0);
      fcvScaleUpPolyu8(nullptr, 0, 0, 0, nullptr, 0, 0, 0);
      fcvScaleUpPolyInterleaveu8(nullptr, 0, 0, 0, nullptr, 0, 0, 0);
      fcvMemDeInit();
      fcvCleanUp();
    }
  ''', dependencies: [fastcv, cdsprpc], name: 'FastCV CDSP scaling')
endif
if fastcv_option.enabled() and not have_fastcv
  error('FastCV headers, scaling functions and libcdsprpc are required')
endif
vpp_dependencies = []
if have_fastcv
  add_project_arguments('-DHAVE_FASTCV=1', language: 'cpp')
  # FastCV/FastRPC are probed above, but loaded with dlopen at runtime.
  vpp_dependencies = [cpp.find_library('dl', required: false)]
endif
summary('FastCV CDSP VPP', have_fastcv)

sources = files(
  'src/backend.cpp',
  'src/decoder.cpp',
  'src/encoder.cpp',
  'src/gpu-copy.cpp',
  'src/h264.cpp',
  'src/hevc.cpp',
  'src/msm.c',
  'src/surface-copy.cpp',
  'src/vp9.cpp',
  'src/vpp.cpp',
)
headers = files('src/internal.hpp')
driver_install_dir = get_option('libdir') / 'dri'

shared_library(
  'v4l2_drv_video',
  sources,
  dependencies: [va, drm, threads, egl, gles, gbm] + vpp_dependencies,
  name_prefix: '',
  install: true,
  install_dir: driver_install_dir,
)

# libva falls back to the DRM driver name on Qualcomm MSM devices.
install_symlink(
  'msm_drv_video.so',
  pointing_to: 'v4l2_drv_video.so',
  install_dir: driver_install_dir,
  install_tag: 'runtime',
)

install_data('README.md', install_dir: get_option('datadir') / 'doc' / meson.project_name())
install_data('LICENSE', install_dir: get_option('datadir') / 'licenses' / meson.project_name())

formatter = find_program('clang-format', required: false)
if formatter.found()
  run_target('format', command: [formatter, '-i', '--style=file', sources, headers])
  run_target('format-check', command: [formatter, '--dry-run', '--Werror', '--style=file', sources, headers])
endif
