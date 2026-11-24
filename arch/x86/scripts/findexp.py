import struct, sys
path = sys.argv[1] if len(sys.argv) > 1 else "/mnt/c/Users/xasko/osito-k/Engine.dll"
base = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x10300000
d = open(path, "rb").read()
pe = struct.unpack_from("<I", d, 0x3c)[0]
nsec = struct.unpack_from("<H", d, pe+6)[0]; optsz = struct.unpack_from("<H", d, pe+20)[0]
secs = []
for i in range(nsec):
    s = pe+24+optsz+i*40
    vma = struct.unpack_from("<I", d, s+12)[0]
    rsz = struct.unpack_from("<I", d, s+16)[0]
    roff = struct.unpack_from("<I", d, s+20)[0]
    secs.append((vma, rsz, roff))
def r2o(rva):
    for vma, rsz, roff in secs:
        if vma <= rva < vma+rsz:
            return roff+(rva-vma)
    return None
edir = struct.unpack_from("<I", d, pe+0x18+96)[0]
nnames = struct.unpack_from("<I", d, r2o(edir)+0x18)[0]
eat, npt, ot = struct.unpack_from("<III", d, r2o(edir)+0x1c)
needles = [n.encode() for n in sys.argv[3:]] or [b"ReadInput", b"PlayerMove", b"ProcessMove"]
for k in range(nnames):
    nrva = struct.unpack_from("<I", d, r2o(npt)+k*4)[0]
    off = r2o(nrva); end = d.find(b"\x00", off); nm = d[off:end]
    if any(x in nm for x in needles):
        o = struct.unpack_from("<H", d, r2o(ot)+k*2)[0]
        frva = struct.unpack_from("<I", d, r2o(eat)+o*4)[0]
        print(nm.decode(), "-> 0x%x" % (base+frva))
