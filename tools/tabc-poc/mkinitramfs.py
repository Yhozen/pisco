#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# mkinitramfs.py <out.cpio> <layer.tar[.gz]>... [--add <path in the image>=<file>]...: an image's layers, applied in
# order, as the kernel's initramfs (an uncompressed newc cpio), the way a container's root is the layers stacked
# (docs/research/tabc-poc.md). OCI whiteouts (.wh.<name>, .wh..wh..opq) remove what lower layers had; hard links
# become copies; every entry is owned as the layer says and dated 0, so the same layers give the same bytes. --add
# puts a file of the host in (the container's /init, a benchmark); /dev/console is added, as the kernel opens it
# before /init.
import sys, tarfile, stat, posixpath

args = sys.argv[1:]; out = args.pop(0); layers, adds = [], []
while args:
    a = args.pop(0)
    if a == '--add': p, _, f = args.pop(0).partition('='); adds.append((p.strip('/'), f))
    else: layers.append(a)

tree = {}   # path (no leading /) -> (mode, uid, gid, data bytes | link target | (major, minor))
def remove(p):
    for k in [k for k in tree if k == p or k.startswith(p + '/')]: del tree[k]
for layer in layers:
    with tarfile.open(layer) as t:
        for m in t:
            p = posixpath.normpath(m.name.lstrip('./') or '.').lstrip('/')
            if p in ('', '.'): continue
            d, b = posixpath.split(p)
            if b == '.wh..wh..opq': [tree.pop(k) for k in [k for k in tree if k.startswith(d + '/')]]; continue
            if b.startswith('.wh.'): remove(posixpath.join(d, b[4:])); continue
            if not m.isdir(): remove(p)
            perm = m.mode & 0o7777
            if m.isdir(): tree[p] = (stat.S_IFDIR | perm, m.uid, m.gid, b'')
            elif m.issym(): tree[p] = (stat.S_IFLNK | 0o777, m.uid, m.gid, m.linkname.encode())
            elif m.islnk(): src = tree[posixpath.normpath(m.linkname.lstrip('./')).lstrip('/')]; tree[p] = (src[0], m.uid, m.gid, src[3])
            elif m.isreg(): tree[p] = (stat.S_IFREG | perm, m.uid, m.gid, t.extractfile(m).read())
            elif m.ischr() or m.isblk(): tree[p] = ((stat.S_IFCHR if m.ischr() else stat.S_IFBLK) | perm, m.uid, m.gid, (m.devmajor, m.devminor))
for p, f in adds: tree[p] = (stat.S_IFREG | 0o755, 0, 0, open(f, 'rb').read())
tree.setdefault('dev', (stat.S_IFDIR | 0o755, 0, 0, b''))
tree['dev/console'] = (stat.S_IFCHR | 0o600, 0, 0, (5, 1))

def entry(o, ino, name, mode, uid, gid, data, rdev=(0, 0)):
    n = name.encode() + b'\0'
    o.write(b'070701' + b''.join(b'%08x' % v for v in (ino, mode, uid, gid, 1, 0, len(data), 0, 0, rdev[0], rdev[1], len(n), 0)))
    o.write(n + b'\0' * (-(110 + len(n)) % 4)); o.write(data + b'\0' * (-len(data) % 4))
with open(out, 'wb') as o:
    # parents before children: sorted paths put a directory before what is in it
    for ino, p in enumerate(sorted(tree), 1):
        mode, uid, gid, v = tree[p]
        if isinstance(v, tuple): entry(o, ino, p, mode, uid, gid, b'', v)
        else: entry(o, ino, p, mode, uid, gid, v)
    entry(o, 0, 'TRAILER!!!', 0, 0, 0, b'')
print(f'{out}: {len(tree)} entries')
