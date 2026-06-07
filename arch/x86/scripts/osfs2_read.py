#!/usr/bin/env python3
# Minimal OsitoFS v2 reader (pure stdlib) — list files / extract a file from an
# image (GPT-wrapped or raw). Finds the partition by scanning for the superblock
# magic. Usage:
#   osfs2_read.py <image> ls
#   osfs2_read.py <image> get <name> [out]
import sys, struct

MAGIC = 0x4F534632  # "OSF2"
FILETAB_OFF = 1 << 20
DATA_OFF    = 4 << 20

def find_part(data):
    # Scan 1MB-aligned offsets for the superblock magic (LE u32 at off+0).
    step = 1 << 20
    for off in range(0, min(len(data), 256 << 20), step):
        if len(data) >= off + 4 and struct.unpack_from('<I', data, off)[0] == MAGIC:
            return off
    # fallback: byte scan
    needle = struct.pack('<I', MAGIC)
    i = data.find(needle)
    return i if i >= 0 else None

def main():
    img, cmd = sys.argv[1], sys.argv[2]
    with open(img, 'rb') as f:
        data = f.read()
    p = find_part(data)
    if p is None:
        print("OSFS2 superblock not found"); return
    magic, ver, bsz, total, used, fcount, nextblk = struct.unpack_from('<7I', data, p)
    print(f"part@0x{p:x} ver={ver} block_size={bsz} files={fcount}")
    ftab = p + FILETAB_OFF
    files = []
    for i in range(4096):
        e = ftab + i*256
        if e+256 > len(data): break
        name = data[e:e+64].split(b'\x00')[0].decode('latin1', 'replace')
        size, = struct.unpack_from('<Q', data, e+64)
        start_block, block_count, crc, flags = struct.unpack_from('<4I', data, e+72)
        if not (flags & 1):  # FLAG_VALID
            continue
        files.append((name, size, start_block, block_count, flags))
    if cmd == 'ls':
        for name, size, sb, bc, fl in sorted(files):
            print(f"  {size:>10}  blk={sb:<6} cnt={bc:<3} fl=0x{fl:x}  {name}")
        print(f"({len(files)} files)")
    elif cmd == 'get':
        target = sys.argv[3]
        for name, size, sb, bc, fl in files:
            if name.lower() == target.lower():
                if fl & 8:  # FLAG_INLINE — data in model_name[128] @ entry+?
                    # inline data location: model_name field. Compute its offset.
                    # entry layout: name[64]+size8+sb4+bc4+crc4+flags4 (=84) + 7*u32 GGUF(28)=112 ; model_name@112
                    e = ftab + files.index((name,size,sb,bc,fl))  # not reliable; recompute below
                print(f"FOUND {name}: size={size} start_block={sb} block_count={bc} flags=0x{fl:x}")
                doff = p + sb * bsz
                blob = data[doff:doff+size]
                print(f"data@0x{doff:x} firstbytes={blob[:16].hex()}")
                out = sys.argv[4] if len(sys.argv) > 4 else None
                if out:
                    with open(out, 'wb') as o: o.write(blob)
                    print(f"wrote {len(blob)} bytes -> {out}")
                return
        print(f"not found: {target}")

main()
