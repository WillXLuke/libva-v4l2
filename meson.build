project(
  'libva-v4l2', 'cpp', 'c',
  version: '0.1.0',
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

sources = files(
  'src/backend.cpp',
  'src/decoder.cpp',
  'src/gpu-copy.cpp',
  'src/h264.cpp',
  'src/hevc.cpp',
  'src/msm.c',
  'src/surface-copy.cpp',
  'src/vp9.cpp',
)
headers = files('src/internal.hpp')
driver_install_dir = get_option('libdir') / 'dri'

shared_library(
  'v4l2_drv_video',
  sources,
  dependencies: [va, drm, threads, egl, gles, gbm],
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
