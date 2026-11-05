#!/usr/bin/env python3
# SPDX-FileCopyrightText: (c) 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# Root filesystem tarball <-> ext4 image, rootless and deterministic, with e2fsprogs (mke2fs, debugfs) only.
#
#     ext4tar.py build <rootfs.tar> <out.img> --size BYTES --epoch T [--label L --uuid U --hash-seed H]
#     ext4tar.py list (--tar <rootfs.tar> | --img <in.img>)     canonical listing on stdout
#     ext4tar.py extract <in.img> <out.tar>                      the image's tree as a canonical tarball
#     ext4tar.py get <rootfs.tar> <path> <out>                   copy one regular file out of a tarball
#
# build: `mke2fs -d` populates the image from a staging copy of the tar, whose files are all owned by the
# caller; one debugfs pass then sets every inode's owner, mode, times and xattrs from the tar headers, so no
# root, chown or loop mount is needed. Times: mtime from the tar (clamped to T, nanoseconds dropped); atime,
# ctime and crtime equal to it; filesystem times are T (E2FSPROGS_FAKE_TIME). The UUID and directory hash seed
# are fixed. mke2fs 1.47 sorts directory entries, so the host's readdir order does not leak in.
#
# The listing (one line per path: type, mode, uid, gid, mtime, size, sha256 or link target, xattrs) is the
# comparison format: two builds are "diff-clean" when their listings match. Character and block devices and
# FIFOs are kept; sockets are refused.

import argparse
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

class Entry:
    # kind: f(ile) d(ir) l(ink) c(har) b(lock) p(ipe); target: symlink target, or 'major,minor' of a device
    __slots__ = ('path', 'kind', 'mode', 'uid', 'gid', 'mtime', 'size', 'target', 'xattrs', 'data')

    def __init__(self, path, kind, mode, uid, gid, mtime, size=0, target='', xattrs=None, data=None):
        self.path, self.kind, self.mode, self.uid, self.gid = path, kind, mode, uid, gid
        self.mtime, self.size, self.target, self.xattrs = mtime, size, target, xattrs or {}
        self.data = data   # host path of the content (regular files)

IFMT = {'f': stat.S_IFREG, 'd': stat.S_IFDIR, 'l': stat.S_IFLNK, 'c': stat.S_IFCHR, 'b': stat.S_IFBLK,
        'p': stat.S_IFIFO}
KIND = {v: k for k, v in IFMT.items()}
TARTYPE = {'d': tarfile.DIRTYPE, 'l': tarfile.SYMTYPE, 'c': tarfile.CHRTYPE, 'b': tarfile.BLKTYPE,
           'p': tarfile.FIFOTYPE}

def norm(name):
    name = name.lstrip('/')
    while name.startswith('./'):
        name = name[2:]
    name = name.rstrip('/')
    return '' if name in ('', '.') else name

def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()

# ---- tar side

def read_tar(path, staging, epoch=None):
    """Entries of a tarball; regular files are copied to staging/<n>. Hard links become the same Entry."""
    entries = {}
    with tarfile.open(path, 'r:') as tf:
        for n, m in enumerate(tf):
            p = norm(m.name)
            mtime = int(m.mtime)
            if epoch is not None:
                mtime = min(mtime, epoch)
            xattrs = {k[len('SCHILY.xattr.'):]: v.encode('utf-8', 'surrogateescape')
                      for k, v in m.pax_headers.items() if k.startswith('SCHILY.xattr.')}
            if m.islnk():
                entries[p] = entries[norm(m.linkname)]
                continue
            if m.isreg():
                data = os.path.join(staging, str(n))
                with tf.extractfile(m) as src, open(data, 'wb') as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
                e = Entry(p, 'f', m.mode, m.uid, m.gid, mtime, m.size, xattrs=xattrs, data=data)
            elif m.isdir():
                e = Entry(p, 'd', m.mode, m.uid, m.gid, mtime, xattrs=xattrs)
            elif m.issym():
                e = Entry(p, 'l', m.mode, m.uid, m.gid, mtime, target=m.linkname, xattrs=xattrs)
            elif m.ischr() or m.isblk():
                e = Entry(p, 'c' if m.ischr() else 'b', m.mode, m.uid, m.gid, mtime,
                          target=f'{m.devmajor},{m.devminor}', xattrs=xattrs)
            elif m.isfifo():
                e = Entry(p, 'p', m.mode, m.uid, m.gid, mtime, xattrs=xattrs)
            else:
                sys.exit(f'ext4tar: {p}: unsupported tar member type {m.type!r}')
            entries[p] = e
    for p in list(entries):   # a tarball need not list parent directories; dpkg and tar create them
        while p:
            p = p.rpartition('/')[0]
            if p in entries:
                break
            entries[p] = Entry(p, 'd', 0o755, 0, 0, epoch or 0)
    entries.setdefault('', Entry('', 'd', 0o755, 0, 0, epoch or 0))
    return entries

def write_tar(entries, path):
    """A canonical tarball: sorted, pax, numeric owners, hard links by first path, no atime/ctime."""
    first = {}
    with tarfile.open(path, 'w', format=tarfile.PAX_FORMAT) as tf:
        for p in sorted(entries):
            e = entries[p]
            ti = tarfile.TarInfo('.' if p == '' else './' + p)
            ti.mode, ti.uid, ti.gid, ti.mtime = e.mode, e.uid, e.gid, e.mtime
            ti.uname = ti.gname = ''
            ti.pax_headers = {'SCHILY.xattr.' + k: v.decode('utf-8', 'surrogateescape')
                              for k, v in sorted(e.xattrs.items())}
            if id(e) in first:
                ti.type, ti.linkname = tarfile.LNKTYPE, first[id(e)]
                tf.addfile(ti)
                continue
            first[id(e)] = ti.name
            if e.kind == 'f':
                ti.type, ti.size = tarfile.REGTYPE, e.size
                with open(e.data, 'rb') as f:
                    tf.addfile(ti, f)
            else:
                ti.type = TARTYPE[e.kind]
                if e.kind == 'l':
                    ti.linkname = e.target
                elif e.kind in 'cb':
                    ti.devmajor, ti.devminor = map(int, e.target.split(','))
                tf.addfile(ti)

# ---- ext4 side (debugfs)

def debugfs(img, cmds, write=False, env=None):
    """Run debugfs commands; returns each command's output, in order."""
    with tempfile.NamedTemporaryFile('w', suffix='.cmds', delete=False) as f:
        f.write(''.join(c + '\n' for c in cmds))
    try:
        r = subprocess.run(['debugfs'] + (['-w'] if write else []) + ['-f', f.name, img], capture_output=True,
                           text=True, errors='surrogateescape', env=env, check=True)
    finally:
        os.unlink(f.name)
    outs, cur = [], None
    for line in r.stdout.splitlines():
        if line.startswith('debugfs: '):
            cur = []
            outs.append(cur)
        elif cur is not None:
            cur.append(line)
    if len(outs) != len(cmds):
        sys.exit(f'ext4tar: debugfs returned {len(outs)} results for {len(cmds)} commands:\n{r.stderr[-2000:]}')
    return outs

def walk_ext4(img):
    """{path: (ino, mode)} for every path in the image, by breadth-first `ls -p`."""
    paths = {'': (2, 0o040755)}
    level = ['']
    while level:
        outs = debugfs(img, [f'ls -p <{paths[d][0]}>' for d in level])
        nxt = []
        for d, out in zip(level, outs):
            for line in out:
                if not line.startswith('/'):
                    continue
                _, ino, mode, _uid, _gid, name = line.split('/', 6)[:6]
                if name in ('', '.', '..') or ino == '0':
                    continue
                p = f'{d}/{name}' if d else name
                paths[p] = (int(ino), int(mode, 8))
                if stat.S_ISDIR(int(mode, 8)):
                    nxt.append(p)
        level = nxt
    return paths

def parse_stat(lines):
    s = {'xattrs': []}
    in_xattrs = False
    for line in lines:
        t = line.strip()
        if t.startswith('User:'):
            f = t.split()
            s['uid'], s['gid'] = int(f[1]), int(f[3])
        elif t.startswith('mtime:'):
            s['mtime'] = int(t.split()[1].split(':')[0], 16)
        elif t.startswith('Fast link dest:'):
            s['target'] = t[len('Fast link dest: "'):-1]
        elif t.startswith('Device major/minor number:'):
            major, minor = t.split()[3].split(':')
            s['rdev'] = f'{int(major)},{int(minor)}'
        elif t.startswith('Extended attributes:'):
            in_xattrs = True
            continue
        elif in_xattrs and line.startswith('  ') and ' (' in t:
            s['xattrs'].append(t.split(' (')[0])
            continue
        in_xattrs = False
    return s

def read_ext4(img, staging):
    paths = walk_ext4(img)
    inos = sorted({ino for ino, _ in paths.values()})
    stats = dict(zip(inos, (parse_stat(o) for o in debugfs(img, [f'stat <{i}>' for i in inos]))))
    modes = {ino: mode for ino, mode in paths.values()}
    dumps, eagets = [], []
    for ino in inos:
        st = stats[ino]
        if stat.S_ISREG(modes[ino]) or (stat.S_ISLNK(modes[ino]) and 'target' not in st):
            dumps.append(f'dump <{ino}> {staging}/{ino}')
        for k, name in enumerate(st['xattrs']):
            eagets.append(f'ea_get -f {staging}/{ino}.x{k} <{ino}> {name}')
    debugfs(img, dumps + eagets)
    byino = {}
    entries = {}
    for p, (ino, mode) in paths.items():
        if p == 'lost+found':
            continue
        if ino not in byino:
            st = stats[ino]
            xattrs = {}
            for k, name in enumerate(st['xattrs']):
                with open(f'{staging}/{ino}.x{k}', 'rb') as f:
                    xattrs[name] = f.read()
            kind = KIND.get(stat.S_IFMT(mode))
            if kind is None:
                sys.exit(f'ext4tar: {p}: sockets are not supported')
            e = Entry(p, kind, stat.S_IMODE(mode), st['uid'], st['gid'], st['mtime'], xattrs=xattrs)
            if kind in 'cb':
                e.target = st['rdev']
            if kind == 'f':
                e.data = f'{staging}/{ino}'
                e.size = os.path.getsize(e.data)
            elif kind == 'l':
                if 'target' in st:
                    e.target = st['target']
                else:
                    with open(f'{staging}/{ino}', 'rb') as f:
                        e.target = f.read().decode('utf-8', 'surrogateescape')
            byino[ino] = e
        entries[p] = byino[ino]
    return entries

def listing(entries):
    out, first = [], {}
    for p in sorted(entries):
        e = entries[p]
        what = sha256(e.data) if e.kind == 'f' else (e.target or '-')
        if e.kind == 'f' and id(e) in first:
            what += ' =' + first[id(e)]
        first.setdefault(id(e), p or '.')
        xa = ','.join(f'{k}={v.hex()}' for k, v in sorted(e.xattrs.items()))
        out.append(f'{p or "."}\t{e.kind}\t{e.mode:04o}\t{e.uid}\t{e.gid}\t{e.mtime}\t{e.size}\t{what}\t{xa}')
    return '\n'.join(out) + '\n'

def build(tar, img, size, epoch, label, uuid, hash_seed, work):
    staging = os.path.join(work, 'data')
    tree = os.path.join(work, 'tree')
    os.makedirs(staging)
    os.makedirs(tree)
    entries = read_tar(tar, staging, epoch)
    # Staging tree: content and structure only, all owned by the caller and writable; metadata comes later.
    made = {}
    for p in sorted(entries):
        e = entries[p]
        if p == '':
            continue
        dst = os.path.join(tree, p)
        if id(e) in made:
            os.link(made[id(e)], dst)
        elif e.kind == 'd':
            os.mkdir(dst, 0o700)
        elif e.kind == 'l':
            os.symlink(e.target, dst)
        elif e.kind == 'f':
            os.link(e.data, dst)
        else:
            continue   # devices and FIFOs: debugfs mknod, below
        made[id(e)] = dst
    env = dict(os.environ, E2FSPROGS_FAKE_TIME=str(epoch), MKE2FS_CONFIG=os.path.join(HERE, 'mke2fs.conf'))
    if os.path.exists(img):
        os.unlink(img)
    with open(img, 'wb') as f:
        f.truncate(size)
    subprocess.run(['mke2fs', '-q', '-t', 'ext4', '-L', label, '-U', uuid,
                    '-E', f'hash_seed={hash_seed},root_owner=0:0,lazy_itable_init=0,lazy_journal_init=0,nodiscard',
                    '-d', tree, img], env=env, check=True)
    nodes = [e for p, e in sorted(entries.items()) if e.kind in 'cbp']
    for e in nodes:
        if any(ch.isspace() for ch in e.path):
            sys.exit(f'ext4tar: {e.path}: device node names with spaces are not supported')
    cmds = []
    for e in nodes:   # debugfs mknod takes a name in the current directory
        parent, _, name = e.path.rpartition('/')
        cmds += [f'cd /{parent}', f'mknod {name} ' + ('p' if e.kind == 'p' else f'{e.kind} ' + e.target.replace(',', ' '))]
    if cmds:
        debugfs(img, cmds, write=True, env=env)
    paths = walk_ext4(img)
    cmds, done = [], set()
    for p in sorted(entries):
        e = entries[p]
        ino, mode = paths[p]
        if ino in done:
            continue
        done.add(ino)
        cmds.append(f'sif <{ino}> mode 0{stat.S_IFMT(mode) | e.mode:o}')
        cmds.append(f'sif <{ino}> uid {e.uid}')
        cmds.append(f'sif <{ino}> gid {e.gid}')
        for t in ('atime', 'ctime', 'mtime', 'crtime'):
            cmds.append(f'sif <{ino}> {t} @{e.mtime}')
            cmds.append(f'sif <{ino}> {t}_extra 0')
        for k, (name, value) in enumerate(sorted(e.xattrs.items())):
            vf = os.path.join(work, f'x{ino}.{k}')
            with open(vf, 'wb') as f:
                f.write(value)
            cmds.append(f'ea_set -f {vf} <{ino}> {name}')
    lf = paths['lost+found'][0]
    for t in ('atime', 'ctime', 'mtime', 'crtime'):
        cmds.append(f'sif <{lf}> {t} @{epoch}')
        cmds.append(f'sif <{lf}> {t}_extra 0')
    debugfs(img, cmds, write=True, env=env)
    r = subprocess.run(['e2fsck', '-fn', img], capture_output=True, text=True, env=env)
    if r.returncode != 0:
        sys.exit(f'ext4tar: e2fsck found problems in {img}:\n{r.stdout[-3000:]}{r.stderr[-1000:]}')

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('tar')
    b.add_argument('img')
    b.add_argument('--size', type=int, required=True)
    b.add_argument('--epoch', type=int, required=True)
    b.add_argument('--label', default='ttsim-root')
    b.add_argument('--uuid', default='74747369-6d00-4000-8000-000000000000')
    b.add_argument('--hash-seed', default='74747369-6d00-4000-8000-000000000001')
    li = sub.add_parser('list')
    g = li.add_mutually_exclusive_group(required=True)
    g.add_argument('--tar')
    g.add_argument('--img')
    x = sub.add_parser('extract')
    x.add_argument('img')
    x.add_argument('tar')
    g = sub.add_parser('get')
    g.add_argument('tar')
    g.add_argument('path')
    g.add_argument('out')
    a = ap.parse_args()
    if a.cmd == 'get':
        with tarfile.open(a.tar, 'r:') as tf:
            for m in tf:
                if norm(m.name) == norm(a.path) and m.isreg():
                    with tf.extractfile(m) as src, open(a.out, 'wb') as dst:
                        shutil.copyfileobj(src, dst, 1 << 20)
                    return
        sys.exit(f'ext4tar: {a.tar}: no regular file {a.path}')
    work = tempfile.mkdtemp(prefix='ext4tar.')
    try:
        if a.cmd == 'build':
            build(a.tar, a.img, a.size, a.epoch, a.label, a.uuid, a.hash_seed, work)
        elif a.cmd == 'list':
            entries = read_tar(a.tar, work) if a.tar else read_ext4(a.img, work)
            sys.stdout.write(listing(entries))
        else:
            write_tar(read_ext4(a.img, work), a.tar)
    finally:
        subprocess.run(['chmod', '-R', 'u+rwx', work])
        shutil.rmtree(work)

if __name__ == '__main__':
    main()
