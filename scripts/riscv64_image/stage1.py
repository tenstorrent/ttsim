#!/usr/bin/env python3
# SPDX-FileCopyrightText: (c) 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# Stage 1 of the ttsim-riscv64 image build: .debs -> the stage-1 root filesystem tarball, on the host.
#
#     stage1.py <debs-dir> <SHA256SUMS> <guest-dir> <epoch> <out.tar>
#     stage1.py --file <deb> <path> <out>      copy one file out of a .deb's data
#
# Every .deb listed in SHA256SUMS (and nothing else) is checked, then its files are extracted into one tree, in
# a canonical tarball (sorted, numeric owners, mtimes clamped to <epoch>). The tree is merged-/usr (bin, sbin,
# lib are symlinks into usr/), as dpkg sees it on a merged system. Nothing is executed: no maintainer script, no
# foreign binary. The tarball also carries /stage2: the .debs themselves, the order to unpack them in, and the
# guest scripts (run, customize), for stage 2 (guest/run) to configure on ttsim-riscv64 itself.
#
# Needs only Python and `zstd` (for data.tar.zst members).

import hashlib
import io
import lzma
import os
import subprocess
import sys
import tarfile
import zlib

MERGED = ('bin', 'sbin', 'lib')
FIRST = ('base-passwd', 'base-files')   # unpacked first: other preinsts look up users and groups

def ar_members(path):
    with open(path, 'rb') as f:
        if f.read(8) != b'!<arch>\n':
            sys.exit(f'stage1: {path}: not a .deb')
        while hdr := f.read(60):
            name, size = hdr[:16].decode().strip().rstrip('/'), int(hdr[48:58])
            yield name, f.read(size)
            f.read(size & 1)

def decompress(name, data):
    if name.endswith('.zst'):
        return subprocess.run(['zstd', '-dcq'], input=data, capture_output=True, check=True).stdout
    if name.endswith('.xz'):
        return lzma.decompress(data)
    if name.endswith('.gz'):
        return zlib.decompress(data, 16 + zlib.MAX_WBITS)
    if name.endswith('.tar'):
        return data
    sys.exit(f'stage1: unsupported member {name}')

def deb_tars(path):
    tars = {}
    for name, data in ar_members(path):
        for kind in ('control', 'data'):
            if name.startswith(kind + '.tar'):
                tars[kind] = tarfile.open(fileobj=io.BytesIO(decompress(name, data)))
    return tars['control'], tars['data']

def control_fields(ctl):
    f = {}
    for line in ctl.extractfile('./control').read().decode().splitlines():
        if line[:1] not in (' ', '\t') and ':' in line:
            k, v = line.split(':', 1)
            f[k] = v.strip()
    return f

def merged(name):
    # ./bin/x -> usr/bin/x, as dpkg writes through the bin -> usr/bin symlink
    name = name.lstrip('/')
    while name.startswith('./'):
        name = name[2:]
    name = '' if name == '.' else name
    head, _, rest = name.partition('/')
    return f'usr/{head}/{rest}' if head in MERGED and rest else name

def deb_file(deb, path, out):
    data = deb_tars(deb)[1]
    for m in data:
        if merged(m.name) == path and m.isreg():
            with open(out, 'wb') as f:
                f.write(data.extractfile(m).read())
            return
    sys.exit(f'stage1: {deb}: no regular file {path}')

def main():
    if sys.argv[1] == '--file':
        return deb_file(*sys.argv[2:5])
    debs, sums, guest, epoch, out = sys.argv[1:6]
    epoch = int(epoch)
    want = {}
    for line in open(sums):
        h, name = line.split()
        want[name] = h
    files = sorted(want)
    for name in files:
        with open(os.path.join(debs, name), 'rb') as f:
            if hashlib.sha256(f.read()).hexdigest() != want[name]:
                sys.exit(f'stage1: {name}: sha256 mismatch')
    tree = {}   # path -> [TarInfo, bytes or None, hard link group (the path of the file's first copy)]

    def put(ti, data=None):
        ti.mtime = min(int(ti.mtime), epoch)
        ti.uname = ti.gname = ''
        group = tree[ti.linkname][2] if ti.islnk() else ti.name
        tree[ti.name] = [ti, data, group]

    for d in ('usr', 'usr/bin', 'usr/sbin', 'usr/lib', 'var', 'var/lib', 'var/lib/dpkg', 'var/lib/dpkg/info',
              'var/lib/dpkg/updates', 'var/lib/dpkg/triggers', 'var/lib/dpkg/alternatives', 'stage2',
              'stage2/debs'):
        ti = tarfile.TarInfo(d)
        ti.type, ti.mode, ti.mtime = tarfile.DIRTYPE, 0o755, epoch
        put(ti)
    for d in MERGED:
        ti = tarfile.TarInfo(d)
        ti.type, ti.linkname, ti.mode, ti.mtime = tarfile.SYMTYPE, f'usr/{d}', 0o777, epoch
        put(ti)
    order = []
    for name in files:
        ctl, data = deb_tars(os.path.join(debs, name))
        order.append((control_fields(ctl)['Package'], name))
        for m in data:
            p = merged(m.name).rstrip('/')
            if not p or p in MERGED:
                continue   # the tree root, and the directories merged-/usr replaces with symlinks
            if p in tree and m.isdir():
                continue   # a directory shipped by several packages: the first one's metadata
            m.name = p
            if m.islnk():
                m.linkname = merged(m.linkname)
            put(m, data.extractfile(m).read() if m.isreg() else None)
    for p, content, mode in [('var/lib/dpkg/status', b'', 0o644), ('var/lib/dpkg/available', b'', 0o644),
                             ('stage2/epoch', f'{epoch}\n'.encode(), 0o644)] + [
                             (f'stage2/{g}', open(os.path.join(guest, g), 'rb').read(),
                              0o755) for g in ('run', 'customize')]:
        ti = tarfile.TarInfo(p)
        ti.mode, ti.mtime, ti.size = mode, epoch, len(content)
        put(ti, content)
    order.sort(key=lambda pn: (FIRST.index(pn[0]) if pn[0] in FIRST else len(FIRST), pn[0]))
    content = ''.join(n + '\n' for _, n in order).encode()
    ti = tarfile.TarInfo('stage2/order')
    ti.mode, ti.mtime, ti.size = 0o644, epoch, len(content)
    put(ti, content)
    for name in files:
        content = open(os.path.join(debs, name), 'rb').read()
        ti = tarfile.TarInfo(f'stage2/debs/{name}')
        ti.mode, ti.mtime, ti.size = 0o644, epoch, len(content)
        put(ti, content)
    for p in list(tree):   # parent directories a .deb leaves out, as dpkg creates them
        while '/' in p:
            p = p.rpartition('/')[0]
            if p in tree:
                break
            ti = tarfile.TarInfo(p)
            ti.type, ti.mode, ti.mtime = tarfile.DIRTYPE, 0o755, epoch
            put(ti)
    # A hard link group's first path in sorted order carries the content; the others link to it.
    first = {}
    with tarfile.open(out, 'w', format=tarfile.PAX_FORMAT) as tf:
        for p in sorted(tree):
            ti, data, group = tree[p]
            ti.name = './' + p
            ti.pax_headers = {k: v for k, v in ti.pax_headers.items() if k.startswith('SCHILY.xattr.')}
            if ti.isreg() or ti.islnk():
                if group in first:
                    ti.type, ti.linkname, ti.size, data = tarfile.LNKTYPE, first[group], 0, None
                else:
                    first[group] = ti.name
                    lead = tree[group][0]
                    ti.type, ti.size, data = tarfile.REGTYPE, lead.size, tree[group][1]
                    ti.mode, ti.uid, ti.gid, ti.mtime = lead.mode, lead.uid, lead.gid, lead.mtime
            tf.addfile(ti, io.BytesIO(data) if data is not None else None)

if __name__ == '__main__':
    main()
