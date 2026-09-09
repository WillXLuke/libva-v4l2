#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Create a local userspace makepkg source archive and print its SHA256 checksum.

After changing sources, refresh PKGBUILD checksums with `makepkg -g`.
"""
import argparse
import gzip
import hashlib
import io
from pathlib import Path
import re
import tarfile


def archive(destination, entries):
    # Fixed metadata makes checksums independent of checkout timestamps/owners.
    with destination.open('wb') as raw, gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0) as gz:
        with tarfile.open(fileobj=gz, mode='w') as tar:
            for name, path in sorted(entries):
                data = path.read_bytes()
                info = tarfile.TarInfo(name)
                info.size = len(data)
                info.mode = 0o644
                tar.addfile(info, io.BytesIO(data))
    print(hashlib.sha256(destination.read_bytes()).hexdigest(), destination)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    version = re.search(r"version:\s*'([^']+)'", (root / 'meson.build').read_text())[1]
    name = 'libva-v4l2-' + version
    paths = [root / p for p in ('meson.build', 'meson_options.txt', 'LICENSE', 'README.md', '.clang-format', '.editorconfig')]
    for directory in ('src', 'patches', 'tests'):
        paths += [p for p in (root / directory).rglob('*') if p.is_file()
                  and '__pycache__' not in p.parts and p.suffix != '.pyc']
    archive(root / 'packaging/arch' / (name + '.tar.gz'), [(name + '/' + str(p.relative_to(root)), p) for p in paths])


if __name__ == '__main__':
    main()
